#include "cache/MaskCache.h"
#include "image/Watermark.h"
#include <iostream>
#include <stdexcept>

namespace {
using namespace sipi;
using namespace sipi::cache;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
MaskPtr mask(std::uint8_t coverage=255) {
    return std::make_shared<const RasterImage8>(RasterImage8{2,1,{coverage,0},{},PixelLayout::gray});
}
}
int main() {
    try {
        const SourceInfo a{"local:a","v1",10}, b{"local:b","v1",10}, c{"local:c","v1",10};
        const auto pixels=mask();
        MaskCache cache({2,4096});
        check(!cache.get(a),"empty miss");
        check(cache.put(a,pixels) && cache.put(b,pixels),"admission");
        const auto bytes=cache.resident_bytes();
        check(cache.put(a,mask(0)) && cache.resident_bytes()==bytes && cache.get(a)==pixels,"first value and promotion");
        check(cache.put(c,pixels) && !cache.get(b) && cache.get(a)==pixels,"LRU eviction");
        for (auto changed : {SourceInfo{"other","v1",10}, SourceInfo{"local:a","v2",10}, SourceInfo{"local:a","v1",11}})
            check(!cache.get(changed),"identity/version/size separation");
        MaskCache probe;
        check(probe.put(a,pixels),"budget probe");
        const auto charge=probe.resident_bytes();
        MaskCache bounded({10,charge});
        check(bounded.put(a,pixels) && bounded.put(b,pixels) && !bounded.get(a) && bounded.size()==1,"byte eviction");
        check(bounded.resident_bytes()==charge,"resident accounting after eviction");
        MaskCache too_small({10,charge-1});
        check(!too_small.put(a,pixels) && !too_small.size(),"oversized declines");
        for (auto limits : {MaskCacheLimits{0,4096},MaskCacheLimits{10,0}}) {
            MaskCache disabled(limits);
            check(!disabled.put(a,pixels) && !disabled.get(a),"disabled");
        }
        MaskPtr retained;
        {
            MaskCache temporary({1,4096});
            temporary.put(a,pixels); retained=temporary.get(a); temporary.put(b,mask(0));
            check(!temporary.get(a),"evicted while retained");
        }
        auto prepared=prepare_watermark(retained,20,10,0.5);
        check(prepared.mask==retained && prepared.mask->pixels.data()==pixels->pixels.data(),"shared preparation without pixel copy");
        bool rejected=false;
        try { cache.put(a,nullptr); } catch (const std::invalid_argument&) { rejected=true; }
        check(rejected,"null rejected");
        rejected=false;
        try { (void)prepare_watermark(MaskPtr{},20,10,0.5); } catch (const WatermarkError&) { rejected=true; }
        check(rejected,"null preparation rejected");
        auto invalid=std::make_shared<RasterImage8>(*pixels); invalid->srgb_intent=0;
        rejected=false;
        try { cache.put(a,invalid); } catch (const std::invalid_argument&) { rejected=true; }
        check(rejected,"colored coverage rejected even on existing key");
        std::cout << "Mask LRU, budgets, source identity and shared lifetime passed\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
