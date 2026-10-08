#pragma once

#include "image/RasterImage.h"
#include "storage/ByteSource.h"
#include <list>
#include <memory>

namespace sipi::cache {
using MaskPtr = std::shared_ptr<const RasterImage8>;

/** Resident bounds for decoded coverage masks; either zero disables retention. */
struct MaskCacheLimits {
    std::size_t max_entries = 64;
    std::size_t max_bytes = 32 * 1024 * 1024;
};

/** Small LRU keyed by freshly opened source identity, version and byte length.
 * Stores only successfully decoded, untagged gray8 coverage masks. No I/O, policy
 * decisions, negative entries or synchronization: callers serialize all access.
 * Immutable shared masks survive eviction and may be read by concurrent workers.
 * Budget counts resident raster/vector capacities, keys and list nodes; allocator
 * overhead, shared_ptr control blocks and values retained after eviction are extra.
 * Old source versions age out normally; no persistence or background thread. */
class MaskCache {
public:
    explicit MaskCache(MaskCacheLimits limits = {});
    MaskCache(const MaskCache&) = delete;
    MaskCache& operator=(const MaskCache&) = delete;
    /** Return a shared hit and promote it to most recently used, or nullptr. */
    [[nodiscard]] MaskPtr get(const SourceInfo& source);
    /** Validate and admit an immutable mask; false if disabled or oversized.
     * Existing keys retain their first value. Invalid input throws invalid_argument;
     * allocation errors propagate without evicting useful entries. Published values
     * must never be mutated through another alias. */
    bool put(const SourceInfo& source, MaskPtr mask);
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t resident_bytes() const noexcept { return bytes_; }
private:
    struct Entry { SourceInfo source; MaskPtr mask; std::size_t charge; };
    std::size_t max_entries_, max_bytes_, bytes_ = 0;
    std::list<Entry> entries_;
};
} // namespace sipi::cache
