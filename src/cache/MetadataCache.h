#pragma once

#include "imgformats/TiffMetadata.h"
#include "imgformats/J2kReader.h"
#include "imgformats/JpegReader.h"
#include "imgformats/PngReader.h"
#include <list>
#include <memory>
#include <variant>

namespace sipi::cache {

/** Original source facts, including ICC data and resolution/directory information.
 * These are not request geometry, authorization decisions or encoded outputs. */
using ImageMetadata = std::variant<TiffMetadata, J2kMetadata, JpegMetadata, PngMetadata>;
using MetadataPtr = std::shared_ptr<const ImageMetadata>;

/** Resident cache bounds; either zero disables admission. */
struct MetadataCacheLimits {
    std::size_t max_entries = 256;
    std::size_t max_bytes = 64 * 1024 * 1024;
};

/** Small LRU of immutable source metadata. No I/O or codec calls.
 * Keys use the opened source's identity, version AND byte size. A new version
 * misses; old versions become ordinary eviction candidates. No negative caching.
 *
 * Callers must serialize access, including stats. IiifWorker uses one external
 * mutex for the shared pool-wide cache; this class performs no synchronization.
 * Shared immutable values may outlive eviction/cache destruction. The byte budget
 * counts resident values, container/key storage and vector capacities (including
 * ICC and nested TIFF extra-sample arrays); allocator overhead and values retained
 * by callers after eviction are additional. Entry count also bounds lookup work.
 * Zero for either limit disables admission. Defaults: 256 entries and 64 MiB.
 * No persistence, time expiry, mutex or background maintenance is needed here.
 */
class MetadataCache {
public:
    explicit MetadataCache(MetadataCacheLimits limits = {});
    MetadataCache(const MetadataCache&) = delete;
    MetadataCache& operator=(const MetadataCache&) = delete;

    /** Hit moves the entry to the front; miss returns nullptr without mutation. */
    [[nodiscard]] MetadataPtr get(const SourceInfo& source);
    /** Admit a successfully read immutable value; false for disabled/oversized.
     * Source fields must be complete and metadata nonnull; otherwise invalid_argument.
     * Allocation errors propagate. Never mutate a published value via another alias.
     * Existing keys retain their first value and become most recently used. */
    bool put(const SourceInfo& source, MetadataPtr metadata);
    [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
    [[nodiscard]] std::size_t resident_bytes() const noexcept { return bytes_; }

private:
    struct Entry { SourceInfo source; MetadataPtr metadata; std::size_t charge; };
    std::size_t max_entries_, max_bytes_, bytes_ = 0;
    std::list<Entry> entries_; // Front is most recently used; at most 256 by default.
};
} // namespace sipi::cache
