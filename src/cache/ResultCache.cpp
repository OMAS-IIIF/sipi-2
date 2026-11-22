#include "ResultCache.h"
#include "storage/LocalFileSource.h"
#include <sqlite3.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <condition_variable>
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
#ifdef SIPI_RESULT_CACHE_TEST_HOOKS
// Defined by the test driver; pauses or fails body I/O without production hooks.
void before_result_cache_body_write();
#endif
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
        if (result != SQLITE_DONE)
            throw CacheError(std::string("Cache database statement failed: ") + sqlite3_errstr(result));
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
/** Include the OS reason (for example "No space left on device") so that an
 * operator can tell a full disk from a permission or hardware problem. */
[[noreturn]] void system_failure(const char* what, int error) {
    throw CacheError(std::string(what) + ": " + std::strerror(error));
}
void sync_file(int fd) {
    int result;
    do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
    if (result < 0) system_failure("Cache synchronization failed", errno);
}
/** Own the temporary pathname even when writing, syncing or closing throws.
 *
 * Publication has two steps so that the slow part runs without the store mutex:
 * write() copies and fsyncs the body (no shared state touched), publish() only
 * renames it to its final name. Until publish() succeeds, the destructor
 * unlinks the temporary file.
 */
class TemporaryFile {
public:
    explicit TemporaryFile(const fs::path& parent) {
        auto pattern = (parent / ".tmp-XXXXXX").string();
        descriptor_.value = ::mkstemp(pattern.data());
        if (descriptor_.value < 0) system_failure("Cache temporary file could not be created", errno);
        path_ = std::move(pattern);
        if (::fcntl(descriptor_.value, F_SETFD, FD_CLOEXEC) < 0)
        {
            ::unlink(path_.c_str());
            throw CacheError("Cache file flags could not be set");
        }
    }
    ~TemporaryFile() { if (!path_.empty()) ::unlink(path_.c_str()); }
    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    /** Write all bytes, make them durable and close the descriptor. */
    void write(std::span<const std::byte> bytes) {
        while (!bytes.empty()) {
            const auto count = ::write(descriptor_.value, bytes.data(),
                std::min(bytes.size(), static_cast<std::size_t>(1048576)));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) system_failure("Cache file write failed", errno);
            if (count == 0) throw CacheError("Cache file write made no progress");
            bytes = bytes.subspan(static_cast<std::size_t>(count));
        }
        // Body data must be durable before its final name can appear: a name
        // surviving a crash must never refer to unwritten blocks.
        sync_file(descriptor_.value);
        const auto fd = std::exchange(descriptor_.value, -1);
        if (::close(fd) != 0) system_failure("Cache file close failed", errno);
    }
    /** Atomically give the written body its final name. Requires write(). */
    void publish(const fs::path& final) {
        if (descriptor_.value >= 0) throw std::logic_error("Cache body was not written");
        fs::rename(path_, final);
        path_.clear();
        // No directory fsync: if a crash loses the rename, recovery drops the
        // index row whose body is missing. Losing a fill is an accepted miss.
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
    /** Bodies admitted by put() but not yet indexed, by digest. Their bytes
     * count against capacity, and a digest in flight is not admitted twice. */
    std::map<std::string, std::uint64_t> publishing;
    std::condition_variable published; // Signalled whenever publishing shrinks.
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
        // WAL with NORMAL synchronization: commits append to the log without an
        // fsync (checkpoints still sync). A power loss may forget the newest
        // rows, never corrupt the index; recovery then removes their bodies as
        // unindexed orphans. This keeps commits cheap while the store mutex is held.
        execute(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL;");
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

    std::uint64_t reserved() const {
        std::uint64_t total = 0;
        for (const auto& [digest, size] : publishing) total += size;
        return total;
    }

    enum class Admission { present, declined, admitted };
    /** First locked phase of put(): decide whether a body may be written and,
     * if so, reserve its bytes. Eviction happens here, before any slow I/O.
     * If another put is writing the same identity, wait for its outcome so
     * that a duplicate still reports "present" instead of writing twice. */
    Admission admit(std::unique_lock<std::mutex>& guard, const CacheKey& key,
                    const ResultMetadata& metadata, std::uint64_t size) {
        const std::string digest(key.digest());
        published.wait(guard, [&] { return !publishing.contains(digest); });
        if (!writable) return Admission::declined;
        if (const auto present = lookup(key, false)) {
            if (present->metadata != metadata || present->bytes->info().size != size)
                throw CacheError("Conflicting cache result for the same identity");
            return Admission::present;
        }
        const auto in_flight = reserved();
        if (size > limits.max_bytes || in_flight > limits.max_bytes - size) return Admission::declined;
        if (usage() > limits.max_bytes - size - in_flight) {
            // A single entry larger than the lower target may occupy the cache
            // alone, provided it still fits the hard maximum.
            const auto needed = size + in_flight;
            const auto desired = target() > needed ? target() - needed : 0;
            if (!evict_to(desired)) return Admission::declined;
        }
        const auto available = fs::space(root).available;
        if (available < disk_headroom || size + in_flight > available - disk_headroom)
            return Admission::declined;
        directory(file(digest).parent_path());
        publishing.emplace(digest, size);
        return Admission::admitted;
    }
    /** Drop a reservation (success or failure) and wake waiting duplicates. */
    void release(const std::string& digest) {
        publishing.erase(digest);
        published.notify_all();
    }

    /** Final locked phase of put(): name the durable body and index it. */
    bool index(const CacheKey& key, const ResultMetadata& metadata,
               std::uint64_t size, TemporaryFile& body) {
        // A concurrent failure may have left unaccounted files; stay read-only.
        if (!writable) return false;
        body.publish(file(key.digest()));
        // A failure after rename leaves a complete orphan; startup
        // reconciliation reclaims it.
        Transaction transaction(database.value);
        Statement insert(database.value,
            "INSERT INTO entries(digest,identity,bytes,media,width,height,last_access) VALUES(?,?,?,?,?,?,?)");
        insert.bind(1, key.digest()); insert.bind(2, key.serialized()); insert.bind(3, size);
        insert.bind(4, metadata.media_type); insert.bind(5, metadata.width);
        insert.bind(6, metadata.height); insert.bind(7, now()); insert.row();
        transaction.commit();
        return true;
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
    auto& state = *impl_;
    const std::string digest(key.digest());
    // Phase 1 (locked): existing entry, capacity, eviction and reservation.
    {
        std::unique_lock guard(state.mutex);
        try {
            switch (state.admit(guard, key, metadata, bytes.size())) {
            case Impl::Admission::present: return true;
            case Impl::Admission::declined: return false;
            case Impl::Admission::admitted: break;
            }
        } catch (...) {
            state.writable = false;
            throw;
        }
    }
    // Phases 2 and 3 release the reservation on every exit. Any failure after
    // admission may leave an unaccounted file, so it also disables writes.
    bool stored = false;
    try {
        // Phase 2 (unlocked): the slow write and fsync. Lookups, hits and other
        // admissions proceed meanwhile; the reservation keeps accounting exact.
        TemporaryFile body(state.file(digest).parent_path());
#ifdef SIPI_RESULT_CACHE_TEST_HOOKS
        before_result_cache_body_write();
#endif
        body.write(bytes);
        // Phase 3 (locked): rename and index. WAL/NORMAL avoids per-commit
        // syncs, but automatic checkpoints can still sync under this lock.
        // The row and the reservation change together, so a woken duplicate
        // sees either the in-flight write or the finished entry.
        const std::lock_guard guard(state.mutex);
        stored = state.index(key, metadata, bytes.size(), body);
        state.release(digest);
    } catch (...) {
        const std::lock_guard guard(state.mutex);
        state.writable = false;
        state.release(digest);
        throw;
    }
    return stored;
}
} // namespace sipi::cache
