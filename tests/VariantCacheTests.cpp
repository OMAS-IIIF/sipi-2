#include "cache/VariantCache.h"
#include <iostream>
#include <stdexcept>
using namespace sipi;
using namespace sipi::cache;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main() {
    try {
        auto image = std::make_shared<const RasterImage8>(RasterImage8{2,1,{0,255},{},PixelLayout::gray});
        VariantCache cache({2,4096});
        check(!cache.get("a"),"empty");
        check(cache.put("a",image) && cache.put("b",image),"admit");
        auto retained=cache.get("a");
        check(cache.put("c",image) && !cache.get("b") && cache.get("a")==retained,"LRU order");
        auto changed = std::make_shared<const RasterImage8>(RasterImage8{1,1,{12},{},PixelLayout::gray});
        check(cache.put("a",changed) && cache.get("a")==retained,"first complete value wins");
        VariantCache probe;
        check(probe.put("a",image),"probe");
        const auto charge=probe.resident_bytes();
        VariantCache bounded({10,charge});
        check(bounded.put("a",image) && bounded.put("b",image) && !bounded.get("a") && bounded.size()==1,"byte eviction");
        check(bounded.resident_bytes()==charge,"accounting");
        VariantCache small({10,charge-1});
        check(!small.put("a",image) && small.size()==0,"oversized");
        for (auto limits : {VariantCacheLimits{0,4096},VariantCacheLimits{10,0}}) {
            VariantCache disabled(limits); check(!disabled.put("a",image),"disabled");
        }
        check(retained->pixels==std::vector<std::uint8_t>({0,255}),"shared immutable lifetime");
        for (const auto& bad : {VariantPtr{},std::make_shared<const RasterImage8>(RasterImage8{2,2,{0},{},PixelLayout::gray})}) {
            bool rejected=false;
            try { cache.put("a",bad); } catch (const std::invalid_argument&) { rejected=true; }
            check(rejected,"invalid raster rejected even on hit");
        }
        std::cout << "Variant LRU, budgets and immutable lifetime passed\n";
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
