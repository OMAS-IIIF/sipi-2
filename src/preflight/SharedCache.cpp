#include "SharedCache.h"
#include <limits>
#include <stdexcept>

namespace sipi::preflight {
SharedCache::SharedCache() : SharedCache(Limits{}) {}
SharedCache::SharedCache(Limits limits, Now now) : limits_(limits), now_(std::move(now)) {
    if (!limits.entries || !limits.bytes) throw std::invalid_argument("Cache limits must be positive");
}
void SharedCache::validate_key(const std::string& key) {
    if (key.empty() || key.size() > 256 || key.find('\0') != std::string::npos)
        throw std::invalid_argument("Cache key must contain 1..256 bytes without NUL");
}
void SharedCache::erase(Entries::iterator entry) {
    bytes_ -= entry->first.size() + entry->second->bytes.size();
    entries_.erase(entry);
}
void SharedCache::prune() {
    const auto now = now_();
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (!it->second->loading && it->second->expires <= now) erase(it++);
        else ++it;
    }
}
bool SharedCache::evict() {
    auto oldest = entries_.end();
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (!it->second->loading && (oldest == entries_.end() || it->second->touched < oldest->second->touched))
            oldest = it;
    }
    if (oldest == entries_.end()) return false;
    erase(oldest);
    return true;
}
std::optional<SharedCache::Value> SharedCache::get(const std::string& key) {
    validate_key(key);
    std::lock_guard lock(mutex_);
    prune();
    const auto found = entries_.find(key);
    if (found == entries_.end() || found->second->loading) return std::nullopt;
    auto& entry = *found->second;
    entry.touched = now_();
    return Value{entry.bytes, entry.revision};
}
SharedCache::Value SharedCache::get_or_load(const std::string& key, const Loader& loader,
                                           Clock::time_point deadline) {
    validate_key(key);
    std::shared_ptr<Entry> entry;
    const auto started = now_();
    {
        std::unique_lock lock(mutex_);
        prune();
        if (const auto found = entries_.find(key); found != entries_.end()) {
            entry = found->second;
            if (!entry->changed.wait_until(lock, deadline, [&] { return !entry->loading; }))
                throw std::runtime_error("Cache loader wait deadline exceeded");
            if (entry->failed) throw std::runtime_error("Cache loader failed");
            if (entry->expires <= now_()) throw std::runtime_error("Cache value expired while waiting");
            entry->touched = now_();
            return {entry->bytes, entry->revision};
        }
        if (key.size() > limits_.bytes) throw std::runtime_error("Cache capacity exceeded");
        while (entries_.size() >= limits_.entries || bytes_ + key.size() > limits_.bytes)
            if (!evict()) throw std::runtime_error("Cache capacity occupied by active loaders");
        entry = std::make_shared<Entry>();
        entries_.emplace(key, entry);
        bytes_ += key.size();
    }
    try {
        auto loaded = loader(); // No cache mutex is held across Lua/network work.
        if (loaded.bytes.size() > 65536 || loaded.ttl.count() < 0 || loaded.ttl.count() > 3600)
            throw std::invalid_argument("Cache value exceeds 64 KiB or TTL is outside 0..3600 seconds");
        // TTL zero shares a completed result only with this flight's callers.
        // It never becomes a resident value and cannot be found by later calls.
        const bool transient = loaded.ttl.count() == 0;
        const auto expires = transient ? Clock::time_point::max() : started + loaded.ttl;
        std::lock_guard lock(mutex_);
        if (expires <= now_()) throw std::runtime_error("Cache TTL elapsed during loading");
        if (key.size() + loaded.bytes.size() > limits_.bytes)
            throw std::runtime_error("Cache value exceeds capacity");
        prune();
        while (bytes_ + loaded.bytes.size() > limits_.bytes)
            if (!evict()) throw std::runtime_error("Cache capacity occupied by active loaders");
        if (revision_ == std::numeric_limits<std::int64_t>::max())
            throw std::runtime_error("Cache revision space exhausted");
        // Prepare the caller's copy before publishing, including allocation failure.
        Value result{loaded.bytes, revision_ + 1};
        entry->bytes = std::move(loaded.bytes);
        bytes_ += entry->bytes.size();
        entry->revision = ++revision_;
        entry->expires = expires;
        entry->touched = now_();
        entry->loading = false;
        if (transient) erase(entries_.find(key));
        entry->changed.notify_all();
        return result;
    } catch (...) {
        std::lock_guard lock(mutex_);
        entry->failed = true;
        entry->loading = false;
        erase(entries_.find(key));
        entry->changed.notify_all();
        throw;
    }
}
bool SharedCache::invalidate(const std::string& key, std::int64_t revision) {
    validate_key(key);
    std::lock_guard lock(mutex_);
    prune();
    const auto found = entries_.find(key);
    if (found == entries_.end() || found->second->loading || found->second->revision != revision) return false;
    erase(found);
    return true;
}
}
