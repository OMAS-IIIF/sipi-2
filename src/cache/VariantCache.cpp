#include "VariantCache.h"
#include <limits>
#include <stdexcept>
#include <utility>

namespace sipi::cache {
VariantCache::VariantCache(VariantCacheLimits limits) : limits_(limits) {}
VariantPtr VariantCache::get(std::string_view key) {
    for (auto it=entries_.begin(); it!=entries_.end(); ++it) {
        if (it->key == key) {
            entries_.splice(entries_.begin(),entries_,it);
            return entries_.front().image;
        }
    }
    return nullptr;
}
bool VariantCache::put(std::string_view key, VariantPtr image) {
    if (key.empty() || !image || !image->width || !image->height || !image->channel_count() ||
        std::uint64_t{image->width}*image->height > std::numeric_limits<std::size_t>::max()/image->channel_count() ||
        std::uint64_t{image->width}*image->height*image->channel_count() != image->pixels.size())
        throw std::invalid_argument("Variant cache requires a complete raster and identity");
    if (!limits_.max_entries || !limits_.max_bytes) return false;
    if (get(key)) return true;
    Entry entry{std::string(key),std::move(image),sizeof(Entry)+2*sizeof(void*)+sizeof(RasterImage8)};
    for (auto part : {entry.key.capacity(),entry.image->pixels.capacity(),entry.image->icc_profile.capacity()}) {
        if (part > std::numeric_limits<std::size_t>::max()-entry.charge) return false;
        entry.charge += part;
    }
    if (entry.charge > limits_.max_bytes) return false;
    entries_.push_front(std::move(entry));
    const auto incoming = entries_.front().charge;
    while (entries_.size()>limits_.max_entries || bytes_>limits_.max_bytes-incoming) {
        bytes_ -= entries_.back().charge;
        entries_.pop_back();
    }
    bytes_ += incoming;
    return true;
}
} // namespace sipi::cache
