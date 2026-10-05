#include "MetadataCache.h"
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace sipi::cache {
namespace {
bool same_source(const SourceInfo& a, const SourceInfo& b) {
    return a.identity == b.identity && a.version == b.version && a.size == b.size;
}
/** Saturate instead of wrapping when estimating storage; overflow is inadmissible. */
void add(std::size_t& total, std::size_t count, std::size_t width = 1) {
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    total = count > (maximum-total)/width ? maximum : total + count*width;
}
std::size_t metadata_bytes(const ImageMetadata& metadata) {
    std::size_t total = sizeof(ImageMetadata);
    std::visit([&](const auto& value) {
        add(total, value.icc_profile.capacity());
        if constexpr (std::is_same_v<std::decay_t<decltype(value)>, TiffMetadata>) {
            add(total, value.image.extra_samples.capacity(), sizeof(std::uint16_t));
            add(total, value.levels.capacity(), sizeof(TiffDirectory));
            for (const auto& level : value.levels)
                add(total, level.extra_samples.capacity(), sizeof(std::uint16_t));
        } else if constexpr (std::is_same_v<std::decay_t<decltype(value)>, J2kMetadata>) {
            add(total, value.components.capacity(), sizeof(J2kComponent));
        }
    }, metadata);
    return total;
}
}
MetadataCache::MetadataCache(MetadataCacheLimits limits)
    : max_entries_(limits.max_entries), max_bytes_(limits.max_bytes) {}

MetadataPtr MetadataCache::get(const SourceInfo& source) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (same_source(it->source, source)) {
            entries_.splice(entries_.begin(), entries_, it);
            return entries_.front().metadata;
        }
    }
    return nullptr;
}
bool MetadataCache::put(const SourceInfo& source, MetadataPtr metadata) {
    if (!metadata || source.identity.empty() || source.version.empty() || !source.size)
        throw std::invalid_argument("Metadata cache requires complete source identity and metadata");
    if (!max_entries_ || !max_bytes_) return false;
    if (get(source)) return true;
    Entry entry{source, std::move(metadata), 0};
    entry.charge = metadata_bytes(*entry.metadata);
    add(entry.charge, sizeof(Entry) + 2*sizeof(void*));
    add(entry.charge, entry.source.identity.capacity());
    add(entry.charge, entry.source.version.capacity());
    if (entry.charge == std::numeric_limits<std::size_t>::max() || entry.charge > max_bytes_) return false;
    // Allocate first: allocation failure must not evict useful existing entries.
    entries_.push_front(std::move(entry));
    const auto incoming = entries_.front().charge;
    while (entries_.size() > max_entries_ || bytes_ > max_bytes_ - incoming) {
        bytes_ -= entries_.back().charge;
        entries_.pop_back();
    }
    bytes_ += incoming;
    return true;
}
} // namespace sipi::cache
