#pragma once
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace sipi::preflight {
/** Process-memory cache scoped to one policy/configuration snapshot, not users.
 * Values are owned serialized bytes; the Lua adapter enforces JSON types.
 * No I/O under the mutex. One loader per key; waiters share its success/failure.
 * Callers must not recursively load through this cache (the Lua adapter enforces it).
 * Expiry is monotonic and starts BEFORE loading, so loading cannot extend TTL.
 * Limits include keys/payload bytes; fixed entry count bounds structural overhead.
 * Destruction requires all operations to have finished. Never log keys/values. */
class SharedCache {
public:
    using Clock = std::chrono::steady_clock;
    struct Limits { std::size_t entries = 256; std::size_t bytes = 4 * 1024 * 1024; };
    struct Value { std::string bytes; std::int64_t revision; };
    struct Loaded { std::string bytes; std::chrono::seconds ttl; };
    using Loader = std::function<Loaded()>;
    using Now = std::function<Clock::time_point()>;
    SharedCache();
    explicit SharedCache(Limits limits, Now now = Clock::now);
    /** Nonblocking lookup; absent/expired/in-flight returns nullopt. Copies bytes. */
    std::optional<Value> get(const std::string& key);
    /** Load on a miss, or wait until the caller's absolute deadline. Exceptions
     * remove the failed flight and wake its waiters; subsequent calls may retry.
     * TTL zero returns to current flight callers without retaining a cache entry.
     * Capacity never evicts active loaders. No stale value is returned. */
    Value get_or_load(const std::string& key, const Loader& loader, Clock::time_point deadline);
    /** Delete only the observed generation. An old 401 cannot erase a new token.
     * Missing, expired, loading or replaced entries return false. */
    bool invalidate(const std::string& key, std::int64_t revision);
private:
    struct Entry {
        std::condition_variable changed;
        bool loading = true;
        bool failed = false;
        std::string bytes;
        std::int64_t revision = 0;
        Clock::time_point expires{}, touched{};
    };
    using Entries = std::map<std::string, std::shared_ptr<Entry>>;
    void erase(Entries::iterator entry);
    void prune();
    bool evict();
    static void validate_key(const std::string& key);
    Limits limits_;
    Now now_;
    std::mutex mutex_;
    Entries entries_;
    std::size_t bytes_ = 0;
    std::int64_t revision_ = 0;
};
}
