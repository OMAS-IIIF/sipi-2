#include "ResultCache.h"
#include "storage/LocalFileSource.h"
#include <sqlite3.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <limits>
#include <map>
#include <mutex>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <utility>

namespace sipi::cache {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t disk_headroom = 64ULL * 1024 * 1024;
constexpr int application_id = 0x53495049; // SIPI, schema v1

struct Descriptor {
    int value = -1;
    ~Descriptor() { if (value >= 0) ::close(value); }
};
struct Database {
    sqlite3* value = nullptr;
    ~Database() { if (value) sqlite3_close(value); }
};
void sql_check(int result) {
    if (result != SQLITE_OK) throw CacheError(std::string("Cache database operation failed: ") + sqlite3_errstr(result));
}
void execute(sqlite3* db, const char* sql) {
    sql_check(sqlite3_exec(db, sql, nullptr, nullptr, nullptr));
}
/** All bound strings are blobs: key bytes can contain NUL and non-UTF8 data. */
class Statement {
public:
    Statement(sqlite3* db, const char* sql) {
        sql_check(sqlite3_prepare_v2(db, sql, -1, &value_, nullptr));
    }
    ~Statement() { sqlite3_finalize(value_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    void bind(int index, std::string_view value) {
        sql_check(sqlite3_bind_blob64(value_, index, value.data(), value.size(), SQLITE_TRANSIENT));
    }
    void bind(int index, std::uint64_t value) {
        if (value > static_cast<std::uint64_t>(INT64_MAX)) throw CacheError("Cache integer overflow");
        sql_check(sqlite3_bind_int64(value_, index, static_cast<sqlite3_int64>(value)));
    }
    bool row() {
        const auto result = sqlite3_step(value_);
        if (result == SQLITE_ROW) return true;
        if (result != SQLITE_DONE) throw CacheError("Cache database statement failed");
        return false;
    }
    std::string bytes(int column) const {
        const auto size = sqlite3_column_bytes(value_, column);
        const auto* data = static_cast<const char*>(sqlite3_column_blob(value_, column));
        return size ? std::string(data, static_cast<std::size_t>(size)) : std::string{};
    }
    std::uint64_t number(int column) const {
        const auto value = sqlite3_column_int64(value_, column);
        if (value < 0) throw CacheError("Invalid cache index integer");
        return static_cast<std::uint64_t>(value);
    }
private:
    sqlite3_stmt* value_ = nullptr;
};
class Transaction {
public:
    explicit Transaction(sqlite3* db) : db_(db) { execute(db_, "BEGIN IMMEDIATE"); }
    ~Transaction() { if (!committed_) sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr); }
    void commit() { execute(db_, "COMMIT"); committed_ = true; }
private:
    sqlite3* db_;
    bool committed_ = false;
};
std::uint64_t now() {
    const auto count = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return count > 0 ? static_cast<std::uint64_t>(count) : 0;
}
bool hex(std::string_view text, std::size_t size) {
    return text.size() == size && text.find_first_not_of("0123456789abcdef") == text.npos;
}
void directory(const fs::path& path) {
    fs::create_directories(path);
    if (fs::symlink_status(path).type() != fs::file_type::directory)
        throw CacheError("Cache requires real directories");
}
void sync_file(int fd) {
    int result;
    do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
    if (result < 0) throw CacheError("Cache synchronization failed");
}
void sync_directory(const fs::path& path) {
    Descriptor fd{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY)};
    if (fd.value < 0) throw CacheError("Cache directory could not be opened");
    sync_file(fd.value);
}
/** Own the temporary pathname even when writing, syncing or closing throws. */
class TemporaryFile {
public:
    explicit TemporaryFile(const fs::path& parent) {
        auto pattern = (parent / ".tmp-XXXXXX").string();
        descriptor_.value = ::mkstemp(pattern.data());
        if (descriptor_.value < 0) throw CacheError("Cache temporary file could not be created");
        path_ = std::move(pattern);
        if (::fcntl(descriptor_.value, F_SETFD, FD_CLOEXEC) < 0)
        {
            ::unlink(path_.c_str());
            throw CacheError("Cache file flags could not be set");
        }
    }
    ~TemporaryFile() { if (!path_.empty()) ::unlink(path_.c_str()); }
    void publish(std::span<const std::byte> bytes, const fs::path& final) {
        while (!bytes.empty()) {
            const auto count = ::write(descriptor_.value, bytes.data(),
                std::min(bytes.size(), static_cast<std::size_t>(1048576)));
            if (count < 0 && errno == EINTR) continue;
            if (count <= 0) throw CacheError("Cache file write failed");
            bytes = bytes.subspan(static_cast<std::size_t>(count));
        }
        sync_file(descriptor_.value);
        const auto fd = std::exchange(descriptor_.value, -1);
        if (::close(fd) != 0) throw CacheError("Cache file close failed");
        fs::rename(path_, final);
        path_.clear();
        sync_directory(final.parent_path());
    }
private:
    Descriptor descriptor_;
    std::string path_;
};
} // namespace

struct ResultCache::Impl {
    fs::path root;
    CacheLimits limits;
    Descriptor lock;
    Database database;
    std::mutex mutex;
    std::map<std::string, std::weak_ptr<const ByteSource>> readers;
    std::map<std::string, std::uint64_t> accesses;
    unsigned hits = 0;
    bool writable = true;

    Impl(const fs::path& path, CacheLimits budget) : root(fs::absolute(path)), limits(budget) {
        if (path.empty() || path.string().find('\0') != std::string::npos || !limits.max_bytes ||
            limits.max_bytes > static_cast<std::uint64_t>(INT64_MAX) / 2 ||
            !limits.target_percent || limits.target_percent >= 100)
            throw CacheError("Invalid cache directory or limits");
        directory(root);
        // macOS temporary directories commonly start with /var -> /private/var.
        // Resolve trusted parent aliases before SQLite's NOFOLLOW open.
        root = fs::canonical(root);
        lock.value = ::open((root / "cache.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock.value < 0 || ::flock(lock.value, LOCK_EX | LOCK_NB) != 0)
            throw CacheError("Cache directory is unavailable or already in use");
        sql_check(sqlite3_open_v2((root / "cache.sqlite3").c_str(), &database.value,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW, nullptr));
        auto* db = database.value;
        const auto scalar = [db](const char* sql) {
            Statement query(db, sql); query.row(); return query.number(0);
        };
        const auto id = scalar("PRAGMA application_id"), schema = scalar("PRAGMA user_version");
        if (id == 0 && schema == 0) {
            if (scalar("SELECT count(*) FROM sqlite_master WHERE name NOT LIKE 'sqlite_%'")) throw CacheError("Cache database belongs to another application");
            execute(db, "BEGIN IMMEDIATE;"
                "CREATE TABLE entries (digest BLOB PRIMARY KEY, identity BLOB NOT NULL, "
                "bytes INTEGER NOT NULL CHECK(bytes>0), media BLOB NOT NULL, "
                "width INTEGER NOT NULL CHECK(width>0), height INTEGER NOT NULL CHECK(height>0), "
                "last_access INTEGER NOT NULL);"
                "CREATE INDEX entries_lru ON entries(last_access);"
                "PRAGMA application_id=1397313609; PRAGMA user_version=1; COMMIT;");
        } else if (id != application_id || schema != 1) {
            throw CacheError("Unsupported cache database identity or version");
        }
        execute(db, "PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;");
        directory(root / "objects");
        recover();
        // Honor a reduced budget on restart, before serving requests.
        if (usage() > limits.max_bytes) evict_to(target());
    }
    fs::path file(std::string_view digest) const {
        if (!hex(digest, 64)) throw CacheError("Invalid cache index digest");
        return root / "objects" / std::string(digest.substr(0, 2)) / (std::string(digest) + ".bin");
    }
    std::uint64_t target() const {
        return limits.max_bytes / 100 * limits.target_percent +
            limits.max_bytes % 100 * limits.target_percent / 100;
    }
    std::uint64_t usage() {
        Statement query(database.value, "SELECT coalesce(sum(bytes),0) FROM entries");
        query.row(); return query.number(0);
    }
    void drop(std::string_view digest) {
        // Remove the file first: a crash can leave a missing-file row, never a
        // ready row pointing at a partially written replacement.
        fs::remove(file(digest));
        Statement query(database.value, "DELETE FROM entries WHERE digest=?");
        query.bind(1, digest); query.row();
        accesses.erase(std::string(digest));
        readers.erase(std::string(digest));
    }
    bool valid_file(std::string_view digest, std::uint64_t size) {
        const auto path = file(digest);
        std::error_code error;
        const auto status = fs::symlink_status(path, error);
        if (error == std::errc::no_such_file_or_directory) return false;
        if (error) throw CacheError("Cache file status failed");
        if (status.type() != fs::file_type::regular) return false;
        return fs::file_size(path) == size;
    }
    /** Reconcile only our reserved hash namespace; never follow symlinks. */
    void recover() {
        for (const auto& shard : fs::directory_iterator(root / "objects"))
            if (hex(shard.path().filename().string(), 2) &&
                shard.symlink_status().type() != fs::file_type::directory)
                throw CacheError("Invalid cache shard directory");
        std::map<std::string, std::uint64_t> ready;
        {
            Statement query(database.value, "SELECT digest,bytes FROM entries");
            while (query.row()) ready.emplace(query.bytes(0), query.number(1));
        }
        for (const auto& [digest, size] : ready)
            if (!valid_file(digest, size)) drop(digest);
        for (const auto& shard : fs::directory_iterator(root / "objects")) {
            if (!hex(shard.path().filename().string(), 2)) continue;
            if (shard.symlink_status().type() != fs::file_type::directory)
                throw CacheError("Invalid cache shard directory");
            for (const auto& entry : fs::directory_iterator(shard.path())) {
                const auto name = entry.path().filename().string();
                if (name.starts_with(".tmp-")) fs::remove(entry.path());
                else if (name.size() == 68 && name.ends_with(".bin") && hex(name.substr(0,64),64)) {
                    const auto digest = name.substr(0,64);
                    if (!ready.contains(digest) || file(digest) != entry.path()) fs::remove(entry.path());
                }
            }
        }
    }
    void flush() {
        if (accesses.empty()) return;
        Transaction transaction(database.value);
        for (const auto& [digest, timestamp] : accesses) {
            Statement query(database.value, "UPDATE entries SET last_access=? WHERE digest=?");
            query.bind(1, timestamp); query.bind(2, digest); query.row();
        }
        transaction.commit();
        accesses.clear(); hits = 0;
    }
    /** Skip pinned files so eviction really frees their accounted disk bytes. */
    bool evict_to(std::uint64_t desired) {
        flush();
        auto total = usage();
        std::vector<std::pair<std::string, std::uint64_t>> candidates;
        {
            Statement query(database.value, "SELECT digest,bytes FROM entries ORDER BY last_access,digest");
            while (query.row()) candidates.emplace_back(query.bytes(0), query.number(1));
        }
        for (const auto& [digest, size] : candidates) {
            if (total <= desired) break;
            if (auto it = readers.find(digest); it != readers.end() && !it->second.expired()) continue;
            drop(digest); total -= size;
        }
        return total <= desired;
    }
    std::optional<CachedResult> lookup(const CacheKey& key, bool touch) {
        std::uint64_t size;
        ResultMetadata metadata;
        {
            Statement query(database.value, "SELECT identity,bytes,media,width,height FROM entries WHERE digest=?");
            query.bind(1, key.digest());
            if (!query.row()) return std::nullopt;
            if (query.bytes(0) != key.serialized()) throw CacheError("Cache digest collision");
            size = query.number(1);
            metadata.media_type = query.bytes(2);
            const auto w = query.number(3), h = query.number(4);
            if (!w || !h || w > UINT32_MAX || h > UINT32_MAX ||
                (metadata.media_type != "image/jpeg" && metadata.media_type != "image/png" && metadata.media_type != "image/webp"))
                throw CacheError("Invalid cached output metadata");
            metadata.width = static_cast<std::uint32_t>(w);
            metadata.height = static_cast<std::uint32_t>(h);
        }
        if (!valid_file(key.digest(), size)) { drop(key.digest()); return std::nullopt; }
        auto& reader = readers[std::string(key.digest())];
        auto source = reader.lock();
        if (!source) {
            try {
                source = std::make_shared<LocalFileSource>(file(key.digest()));
            } catch (const ByteSourceError& error) {
                if (error.code() == ByteSourceErrorCode::not_found) {
                    drop(key.digest());
                    return std::nullopt;
                }
                throw CacheError("Cached result could not be opened");
            }
            if (source->info().size != size) { drop(key.digest()); return std::nullopt; }
            reader = source;
        }
        if (touch) {
            accesses[std::string(key.digest())] = now();
            if (++hits >= 64) flush();
        }
        // Expired weak entries need not accumulate with the number of all hits.
        std::erase_if(readers, [](const auto& pair) { return pair.second.expired(); });
        return CachedResult{std::move(metadata), std::move(source)};
    }
};

ResultCache::ResultCache(const fs::path& directory, CacheLimits limits)
    : impl_(std::make_unique<Impl>(directory, limits)) {}
ResultCache::~ResultCache() {
    try { impl_->flush(); } catch (...) { /* Disposable last-use information. */ }
}
std::optional<CachedResult> ResultCache::get(const CacheKey& key) {
    const std::lock_guard guard(impl_->mutex);
    return impl_->lookup(key, true);
}
void ResultCache::flush_accesses() {
    const std::lock_guard guard(impl_->mutex);
    impl_->flush();
}
bool ResultCache::put(const CacheKey& key, const ResultMetadata& metadata, std::span<const std::byte> bytes) {
    if (bytes.empty() || !metadata.width || !metadata.height ||
        (metadata.media_type != "image/jpeg" && metadata.media_type != "image/png" && metadata.media_type != "image/webp"))
        throw CacheError("Invalid encoded cache result");
    const std::lock_guard guard(impl_->mutex);
    auto& state = *impl_;
    if (!state.writable) return false;
    try {
        if (const auto present = state.lookup(key, false)) {
            if (present->metadata != metadata || present->bytes->info().size != bytes.size())
                throw CacheError("Conflicting cache result for the same identity");
            return true;
        }
        if (bytes.size() > state.limits.max_bytes) return false;
        if (state.usage() > state.limits.max_bytes - bytes.size()) {
            // A single entry larger than the lower target may occupy the cache
            // alone, provided it still fits the hard maximum.
            const auto desired = state.target() > bytes.size() ? state.target() - bytes.size() : 0;
            if (!state.evict_to(desired)) return false;
        }
        const auto available = fs::space(state.root).available;
        if (available < disk_headroom || bytes.size() > available - disk_headroom) return false;
        const auto final = state.file(key.digest());
        directory(final.parent_path());
        TemporaryFile temporary(final.parent_path());
        temporary.publish(bytes, final);
        // No transaction is held while writing encoded bytes. A failure after
        // rename leaves a complete orphan; startup reconciliation reclaims it.
        Transaction transaction(state.database.value);
        Statement insert(state.database.value,
            "INSERT INTO entries(digest,identity,bytes,media,width,height,last_access) VALUES(?,?,?,?,?,?,?)");
        insert.bind(1, key.digest()); insert.bind(2, key.serialized()); insert.bind(3, bytes.size());
        insert.bind(4, metadata.media_type); insert.bind(5, metadata.width);
        insert.bind(6, metadata.height); insert.bind(7, now()); insert.row();
        transaction.commit();
        return true;
    } catch (...) {
        state.writable = false;
        throw;
    }
}
} // namespace sipi::cache
