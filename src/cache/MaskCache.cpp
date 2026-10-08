#include "MaskCache.h"
#include <limits>
#include <stdexcept>
#include <utility>

namespace sipi::cache {
namespace {
/** Saturating accounting prevents wrapped charges from admitting oversized values. */
void add(std::size_t& total, std::size_t amount) {
    const auto maximum = std::numeric_limits<std::size_t>::max();
    total = amount > maximum-total ? maximum : total+amount;
}
}
MaskCache::MaskCache(MaskCacheLimits limits)
    : max_entries_(limits.max_entries), max_bytes_(limits.max_bytes) {}

MaskPtr MaskCache::get(const SourceInfo& source) {
    for (auto it=entries_.begin(); it!=entries_.end(); ++it) {
        if (it->source.identity==source.identity && it->source.version==source.version &&
            it->source.size==source.size) {
            entries_.splice(entries_.begin(),entries_,it);
            return entries_.front().mask;
        }
    }
    return nullptr;
}

bool MaskCache::put(const SourceInfo& source, MaskPtr mask) {
    if (source.identity.empty() || source.version.empty() || !source.size || !mask ||
        !mask->width || !mask->height || mask->layout!=PixelLayout::gray ||
        std::uint64_t{mask->width}*mask->height!=mask->pixels.size() ||
        !mask->icc_profile.empty() || mask->srgb_intent)
        throw std::invalid_argument("Mask cache requires a complete source and untagged gray8 mask");
    if (!max_entries_ || !max_bytes_) return false;
    if (get(source)) return true;
    Entry entry{source,std::move(mask),sizeof(Entry)+2*sizeof(void*)+sizeof(RasterImage8)};
    add(entry.charge,entry.source.identity.capacity());
    add(entry.charge,entry.source.version.capacity());
    add(entry.charge,entry.mask->pixels.capacity());
    add(entry.charge,entry.mask->icc_profile.capacity());
    if (entry.charge==std::numeric_limits<std::size_t>::max() || entry.charge>max_bytes_) return false;
    entries_.push_front(std::move(entry));
    const auto incoming=entries_.front().charge;
    while (entries_.size()>max_entries_ || bytes_>max_bytes_-incoming) {
        bytes_-=entries_.back().charge;
        entries_.pop_back();
    }
    bytes_+=incoming;
    return true;
}
} // namespace sipi::cache
