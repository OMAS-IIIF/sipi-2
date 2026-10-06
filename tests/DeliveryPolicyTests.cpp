#include "http/DeliveryPolicy.h"
#include "image/OutputLimits.h"
#include <iostream>

namespace {
using namespace sipi;
using namespace sipi::http;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    try { f(); } catch (const DeliveryLimitError&) { return; }
    throw std::runtime_error("Expected working-budget rejection");
}
void run() {
    const auto defaults = bounded_output_policy({});
    check(defaults.max_width==8192 && defaults.max_height==8192 &&
          defaults.max_area==8388608, "public defaults");
    check(bounded_output_policy({.max_width=100}).max_height==100, "height inherits width");
    bool invalid=false;
    try { (void)bounded_output_policy({.max_area=8388609}); }
    catch (const std::invalid_argument&) { invalid=true; }
    check(invalid,"hard ceiling validation");

    // A huge master is eligible when the actual pyramid plan reads a small level.
    TiffDirectory base;
    base.width=100000; base.height=100000;
    base.samples_per_pixel=3; base.bits_per_sample=8; base.sample_format=1;
    base.photometric=2; base.planar_config=1; base.compression=1;
    base.layout=TiffTiles{256,256};
    auto small=base; small.width=1000; small.height=1000; small.offset=10;
    TiffMetadata tiff{base,{small},{}};
    const iiif::ImageGeometry thumbnail{{0,0,100000,100000},1000,1000};
    validate_delivery_budget(tiff,thumbnail,{false,0});
    tiff.levels.clear();
    rejects([&]{ validate_delivery_budget(tiff,thumbnail,{false,0}); });
    // Small ROI but enormous physical block is still expensive.
    tiff.image.layout=TiffStrips{100000};
    rejects([&]{ validate_delivery_budget(tiff,{{0,0,10,10},10,10},{false,0}); });

    // Progressive coefficient storage must count even for a tiny final request.
    JpegMetadata jpeg{100,100,JpegColor::rgb,true,{}};
    const iiif::ImageGeometry ten{{0,0,100,100},10,10};
    jpeg.coefficient_bytes=256*1024*1024;
    rejects([&]{ validate_delivery_budget(jpeg,ten,{false,0}); });
    jpeg.coefficient_bytes=100000;
    validate_delivery_budget(jpeg,ten,{false,0});

    PngMetadata png; png.width=100000; png.height=100000;
    rejects([&]{ validate_delivery_budget(png,thumbnail,{false,0}); });
    png.width=32768; png.height=1;
    const iiif::ImageGeometry thin{{0,0,32768,1},32768,1};
    validate_delivery_budget(png,thin,{false,90});
    rejects([&]{ validate_delivery_budget(png,thin,{false,45}); });

    J2kMetadata j2k{1000,1000,0,0,1000,1000,1,{{1000,1000,8,1,1,false}},
                    {},J2kColor::gray};
    validate_delivery_budget(j2k,{{0,0,1000,1000},1000,1000},{false,0});
    // Whole reduced codec tile allocation counts, even for a small ROI.
    j2k.tile_width=100000; j2k.tile_height=100000;
    rejects([&]{ validate_delivery_budget(j2k,{{0,0,10,10},10,10},{false,0}); });
}
}
int main() {
    try { run(); std::cout<<"Delivery budget and policy checks passed\n"; }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
