#include "cache/ResultCache.h"
#include <sqlite3.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace sipi::cache;
void require(bool value, const char* what) { if (!value) throw std::runtime_error(what); }
struct Directory {
    fs::path path;
    Directory() {
        auto pattern = (fs::temp_directory_path() / "sipi-cache-test-XXXXXX").string();
        require(::mkdtemp(pattern.data()), "mkdtemp"); path = pattern;
    }
    ~Directory() { std::error_code error; fs::remove_all(path, error); }
};
CacheKey key(std::string version = "1") {
    RenderIdentity identity;
    identity.source = {"local:test", std::move(version), 100};
    identity.variant = "original";
    identity.geometry = {{0,0,10,10},10,10};
    identity.rotation = {false,0};
    identity.quality = sipi::iiif::Quality::default_quality;
    identity.input_layout = sipi::PixelLayout::rgb;
    identity.encoding = PngEncoding{};
    return CacheKey(identity);
}
const ResultMetadata metadata{"image/png",10,10};
const std::array<std::byte, 4> payload{std::byte{1},std::byte{0},std::byte{3},std::byte{255}};
fs::path file(const fs::path& root, const CacheKey& value) {
    return root / "objects" / std::string(value.digest().substr(0,2)) / (std::string(value.digest())+".bin");
}
void sql(const fs::path& root, const char* command) {
    sqlite3* db = nullptr;
    require(sqlite3_open((root/"cache.sqlite3").c_str(), &db) == SQLITE_OK, "test database open");
    const auto result = sqlite3_exec(db, command, nullptr, nullptr, nullptr);
    sqlite3_close(db);
    require(result == SQLITE_OK, "test SQL");
}
template<class Function> void rejects(Function function, const char* what) {
    bool failed = false;
    try { function(); } catch (const CacheError&) { failed = true; }
    require(failed, what);
}
void verify(const CachedResult& hit) {
    require(hit.metadata == metadata, "metadata round trip");
    std::array<std::byte,4> bytes{};
    require(hit.bytes->read_at(0, bytes) == bytes.size() && bytes == payload, "binary bytes round trip");
    require(hit.bytes->read_at(4, bytes) == 0, "EOF");
}
void persistence() {
    Directory dir;
    const auto first = key();
    std::optional<CachedResult> retained;
    {
        ResultCache store(dir.path);
        require(!store.get(first), "initial miss");
        require(store.put(first, metadata, payload), "first publication");
        retained = store.get(first); require(retained.has_value(), "first hit"); verify(*retained);
        require(store.put(first, metadata, payload), "idempotent publication");
        require(!store.get(key("replaced")), "source version miss");
        rejects([&] { ResultCache other(dir.path); }, "exclusive instance lock");
        store.flush_accesses();
    }
    verify(*retained);
    { ResultCache store(dir.path); const auto hit = store.get(first); require(hit.has_value(), "restart hit"); verify(*hit); }
    require(fs::exists(dir.path/"cache.sqlite3"), "fixed database filename");
}
void recovery() {
    Directory dir;
    const auto first = key(), orphan = key("orphan");
    { ResultCache store(dir.path); require(store.put(first, metadata, payload), "seed recovery"); }
    // Reproduce interruption before rename, after rename/before index, and loss
    // of a previously indexed body. Recovery must not publish any of them.
    fs::create_directories(file(dir.path, orphan).parent_path());
    std::ofstream(file(dir.path, orphan)) << "unindexed";
    const auto temporary = file(dir.path, orphan).parent_path()/".tmp-abandoned";
    std::ofstream(temporary) << "partial";
    std::ofstream(file(dir.path, orphan).parent_path()/"operator-note.txt") << "keep";
    fs::remove(file(dir.path, first));
    {
        ResultCache store(dir.path);
        require(!store.get(first) && !store.get(orphan), "recovery misses");
        require(!fs::exists(temporary) && !fs::exists(file(dir.path, orphan)), "orphan cleanup");
        require(fs::exists(temporary.parent_path()/"operator-note.txt"), "unrelated file preserved");
        require(store.put(first, metadata, payload), "repopulate after recovery");
        fs::resize_file(file(dir.path, first), 1);
        require(!store.get(first), "truncated cached file is a miss");
    }
    // A committed result survives abrupt process termination (no C++ destructors).
    const auto pid = ::fork(); require(pid >= 0, "fork");
    if (pid == 0) {
        try { ResultCache store(dir.path); const bool ok = store.put(first, metadata, payload); ::_exit(ok ? 0 : 1); }
        catch (...) { ::_exit(2); }
    }
    int status = 0; require(::waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "crash writer");
    ResultCache store(dir.path); require(store.get(first).has_value(), "committed result survives abrupt exit");
}
void capacity() {
    Directory dir;
    const auto a=key("a"), b=key("b"), c=key("c");
    {
        ResultCache store(dir.path, {10,80});
        require(store.put(a,metadata,payload) && store.put(b,metadata,payload), "fill budget");
        sql(dir.path, "UPDATE entries SET last_access=0");
        { auto recent = store.get(a); require(recent.has_value(), "LRU touch"); }
        require(store.put(c,metadata,payload), "admit after eviction");
        require(store.get(a).has_value() && !store.get(b) && store.get(c).has_value(), "least recently used evicted");
        const std::array<std::byte,11> oversized{};
        require(!store.put(key("huge"), metadata, oversized), "oversized bypass");
    }
    { ResultCache store(dir.path, {4,90}); require(!store.get(a) && !store.get(c), "restart applies smaller limit/target"); }
    Directory pinned;
    {
        ResultCache store(pinned.path, {8,90});
        require(store.put(a,metadata,payload) && store.put(b,metadata,payload), "pin seeds");
        auto one=store.get(a), two=store.get(b);
        require(!store.put(c,metadata,payload), "pinned entries block admission");
        verify(*one); verify(*two);
        one.reset(); two.reset();
        require(store.put(c,metadata,payload), "released pins permit eviction");
        require(!store.get(a) && !store.get(b), "hysteresis frees to lower target");
    }
}
void failures() {
    Directory dir;
    const auto first = key();
    {
        ResultCache store(dir.path);
        rejects([&] { (void)store.put(first, metadata, {}); }, "empty encoding rejected");
        require(store.put(first,metadata,payload), "invalid input does not disable store");
    }
    sql(dir.path, "UPDATE entries SET identity=x'626164'");
    {
        ResultCache store(dir.path);
        rejects([&] { (void)store.get(first); }, "collision lookup fails closed");
        rejects([&] { (void)store.put(first,metadata,payload); }, "collision cannot overwrite");
    }
    Directory broken;
    {
        ResultCache store(broken.path);
        sql(broken.path, "CREATE TRIGGER fail_insert BEFORE INSERT ON entries BEGIN SELECT RAISE(FAIL,'test'); END;");
        rejects([&] { (void)store.put(first,metadata,payload); }, "index publication failure");
        require(!store.get(first), "failed commit invisible");
        require(!store.put(key("another"),metadata,payload), "uncertain accounting disables writes");
    }
    sql(broken.path,"DROP TRIGGER fail_insert");
    {
        ResultCache store(broken.path);
        require(!fs::exists(file(broken.path,first)), "failed commit orphan removed");
        require(store.put(first,metadata,payload), "reopen restores writes");
    }
    sql(broken.path,"PRAGMA user_version=999");
    rejects([&] { ResultCache store(broken.path); }, "unknown schema preserved/rejected");
    require(fs::exists(file(broken.path,first)), "unsupported schema does not delete data");
    Directory symlink;
    fs::create_directories(symlink.path/"objects");
    fs::create_directory_symlink(dir.path, symlink.path/"objects"/"aa");
    rejects([&] { ResultCache store(symlink.path); }, "no traversal through shard symlinks");
    rejects([&] { ResultCache store(dir.path,{0,90}); }, "zero limit");
    rejects([&] { ResultCache store(dir.path,{100,100}); }, "invalid cleanup target");
}
void concurrency() {
    Directory dir;
    ResultCache store(dir.path);
    std::vector<std::future<void>> tasks;
    for (unsigned i=0; i<8; ++i) tasks.push_back(std::async(std::launch::async,[&,i] {
        for (unsigned j=0; j<16; ++j) {
            const auto same = key();
            require(store.put(same,metadata,payload), "concurrent duplicate");
            const auto different = key(std::to_string(i+10));
            require(store.put(different,metadata,payload), "concurrent distinct");
            auto hit = store.get(same); require(hit.has_value(), "concurrent hit"); verify(*hit);
        }
    }));
    for (auto& task : tasks) task.get();
}
}
int main() {
    try {
        std::cout << "persistence" << std::endl; persistence();
        std::cout << "recovery" << std::endl; recovery();
        std::cout << "capacity" << std::endl; capacity();
        std::cout << "failures" << std::endl; failures();
        std::cout << "concurrency" << std::endl; concurrency();
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "Persistent cache tests passed\n";
}
