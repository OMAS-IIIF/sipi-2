#include "cache/MetadataCache.h"
#include <iostream>
#include <stdexcept>

namespace {
using namespace sipi;
using namespace sipi::cache;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
MetadataPtr tiff(std::size_t profile_capacity = 0) {
    TiffMetadata value;
    value.image.width = 100; value.image.height = 80;
    value.image.extra_samples = {2};
    value.levels.push_back(value.image);
    value.levels[0].width = 50;
    value.icc_profile.reserve(profile_capacity);
    return std::make_shared<const ImageMetadata>(std::move(value));
}
void exercise() {
    const SourceInfo a{"local:a","v1",100}, b{"local:b","v1",100}, c{"local:c","v1",100};
    MetadataCache cache({2, 1024*1024});
    require(!cache.get(a), "empty miss");
    const auto first = tiff();
    require(cache.put(a, first), "admit TIFF");
    require(cache.get(a) == first, "immutable shared value, no deep hit copy");
    require(!cache.get({a.identity,"v2",100}), "version miss");
    require(!cache.get({a.identity,a.version,101}), "size miss");
    require(!cache.get({"s3:a",a.version,100}), "backend identity miss");
    J2kMetadata jp2{};
    jp2.width = 37; jp2.height = 23; jp2.resolution_count = 4;
    jp2.components = {{37,23,8,1,1,false}};
    auto second = std::make_shared<const ImageMetadata>(std::move(jp2));
    require(cache.put(b,second), "admit JPEG 2000");
    auto retained = cache.get(a); // b is now least recently used.
    require(cache.put(c,tiff()) && !cache.get(b) && cache.get(a), "LRU count eviction");
    require(cache.size()==2, "entry count bounded");
    require(cache.put(a,tiff(1000)) && cache.get(a)==first, "same version retains first value");
    const auto charge = cache.resident_bytes()/2;
    MetadataCache bytes({10,charge});
    require(bytes.put(a,first) && bytes.put(b,first), "byte-limited admission");
    require(bytes.size()==1 && !bytes.get(a) && bytes.get(b), "byte eviction");
    require(bytes.resident_bytes() <= charge, "accounted bytes bounded");
    require(!bytes.put(c,tiff(charge+1)), "reserved ICC capacity counts even with zero size");
    require(bytes.get(b)!=nullptr, "oversized admission does not evict good entry");
    require(!MetadataCache({0,1024}).put(a,first), "zero entries disables");
    require(!MetadataCache({10,0}).put(a,first), "zero bytes disables");
    bool rejected = false;
    try { cache.put({"","",0},first); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "incomplete source rejected");
    rejected = false;
    try { cache.put(a,nullptr); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "null metadata rejected");
    require(cache.put(b,second) && cache.put(c,second), "evict original");
    require(std::get<TiffMetadata>(*retained).levels[0].width==50, "eviction preserves retained snapshot");
    MetadataPtr surviving;
    { MetadataCache temporary; temporary.put(a,first); surviving=temporary.get(a); }
    require(std::get<TiffMetadata>(*surviving).image.width==100, "snapshot outlives cache");
    // Nested allocations participate in accounting, independently of ICC data.
    MetadataCache sizes;
    sizes.put(a, first);
    const auto small = sizes.resident_bytes();
    TiffMetadata larger;
    larger.levels.reserve(100);
    larger.image.extra_samples.reserve(100);
    larger.levels.push_back(larger.image);
    sizes.put(b, std::make_shared<const ImageMetadata>(std::move(larger)));
    require(sizes.resident_bytes() > 2*small, "TIFF directory capacities counted");
    MetadataCache components;
    components.put(a,second);
    const auto compact = components.resident_bytes();
    J2kMetadata expanded{}; expanded.components.reserve(100);
    components.put(b,std::make_shared<const ImageMetadata>(std::move(expanded)));
    require(components.resident_bytes() > 2*compact, "J2K component capacities counted");
}
}
int main() {
    try { exercise(); }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
    std::cout << "Metadata cache ownership, versioning, LRU and budgets passed\n";
}
