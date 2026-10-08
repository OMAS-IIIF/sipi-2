#pragma once
#include "image/RasterImage.h"
#include <list>
#include <memory>
#include <string>
#include <string_view>

namespace sipi::cache {
using VariantPtr = std::shared_ptr<const RasterImage8>;
/** Pool-wide resident bounds. Either zero disables retention. */
struct VariantCacheLimits {
    std::size_t max_entries = 16;
    std::size_t max_bytes = 64 * 1024 * 1024;
};
/** LRU of complete reduced public rasters, before watermark/IIIF transformations.
 * The caller supplies a full serialized, versioned identity, not just its hash.
 * Caller serializes access; the cache itself performs no I/O or policy execution.
 * Published pixels are immutable. Active references can outlive eviction; their
 * memory and allocator/control-block overhead are outside the resident budget.
 * No persistence: restart regenerates derivatives; encoded result cache is separate.
 */
class VariantCache {
public:
    explicit VariantCache(VariantCacheLimits limits = {});
    /** Promote a hit; nullptr on miss. */
    [[nodiscard]] VariantPtr get(std::string_view key);
    /** Admit a complete raster; false if disabled/oversized. Invalid inputs throw.
     * Allocation failures propagate without invalidating existing entries. */
    bool put(std::string_view key, VariantPtr image);
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t resident_bytes() const noexcept { return bytes_; }
private:
    struct Entry { std::string key; VariantPtr image; std::size_t charge; };
    VariantCacheLimits limits_;
    std::size_t bytes_ = 0;
    std::list<Entry> entries_;
};
} // namespace sipi::cache
