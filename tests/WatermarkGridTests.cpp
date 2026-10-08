#include "image/Watermark.h"
#include "image/AreaResize.h"
#include "imgformats/TiffReadPlan.h"
#include "imgformats/J2kReadPlan.h"
#include <array>
#include <iostream>
#include <limits>

namespace {
using namespace sipi;
constexpr std::array<std::uint8_t,1> white{255};
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
RasterImage8 black(std::uint32_t w, std::uint32_t h) {
    return {w,h,std::vector<std::uint8_t>(static_cast<std::size_t>(w)*h),{},PixelLayout::gray};
}
void compare_crop(const PreparedWatermark& mark, const WatermarkGrid& grid,
                  std::uint32_t w, std::uint32_t h) {
    const auto whole=apply_watermark(black(grid.width,grid.height),mark,white,0.63,
                                    WatermarkGrid{grid.width,grid.height});
    const auto crop=apply_watermark(black(w,h),mark,white,0.63,grid);
    for (unsigned y=0;y<h;++y) for (unsigned x=0;x<w;++x)
        check(crop.pixels[y*w+x]==whole.pixels[(grid.origin_y+y)*grid.width+grid.origin_x+x],
              "Level crop differs from complete level");
}
void run() {
    auto source=black(3,2); source.pixels={0,70,255,255,110,0};
    const auto mark=prepare_watermark(source,17,13,0.6);
    // Direct source sampling should be independent of how the level is tiled.
    for (unsigned h=1;h<=13;++h) for (unsigned w=1;w<=17;++w) {
        // Partition every level into rows and two horizontal pieces, including
        // odd dimensions and edges cutting through the watermark.
        for (unsigned y=0;y<h;++y) {
            const auto split=w/2;
            if (split) compare_crop(mark,{w,h,0,y},split,1);
            compare_crop(mark,{w,h,split,y},w-split,1);
        }
    }
    // Aligned full-canvas placement: independent resize implementation checks
    // reduction, enlargement and mixed-axis filtering directly from the source.
    auto original=black(5,3);
    original.pixels={0,255,0,255,0, 255,0,255,0,255, 20,40,80,160,240};
    const PreparedWatermark aligned{std::make_shared<const RasterImage8>(original),23,19,0,0,23,19};
    for (unsigned h=1;h<=19;++h) for (unsigned w=1;w<=23;++w) {
        const auto expected=resize_image(original,{{0,5,1},{0,3,1}},w,h);
        const auto actual=apply_watermark(black(w,h),aligned,white,1,WatermarkGrid{w,h});
        for (std::size_t i=0;i<actual.pixels.size();++i)
            check(std::abs(int(actual.pixels[i])-int(expected.pixels[i]))<=1,"Direct source resize oracle");
    }
    // Mean of a checkerboard remains 128, even on a huge virtual canvas.
    auto checker=black(2,2); checker.pixels={0,255,255,0};
    auto large=prepare_watermark(checker,UINT32_MAX,UINT32_MAX,1,4);
    check(apply_watermark(black(1,1),large,white,1,WatermarkGrid{1,1},5).pixels[0]==128,
          "Direct reduction across huge placement");
    auto one=black(1,1); one.pixels[0]=255;
    const auto centered=prepare_watermark(one,3,1,1.0/3);
    const auto halves=apply_watermark(black(2,1),centered,white,1,WatermarkGrid{2,1});
    check(halves.pixels==std::vector<std::uint8_t>({85,85}),"Fractional edge coverage");
    RasterImage8 transparent{2,1,{77,0,99,0},{},PixelLayout::gray_alpha};
    const auto alpha=apply_watermark(transparent,centered,white,1,WatermarkGrid{2,1});
    check(alpha.pixels==std::vector<std::uint8_t>({255,85,255,85}),"Reduced straight alpha");

    // Exercise the actual codec planners' enclosing crops and fractional windows.
    TiffDirectory base; base.width=17; base.height=13;
    TiffDirectory level; level.width=9; level.height=7;
    const iiif::ImageGeometry geometry{{3,2,11,9},4,3};
    const auto tiff=plan_tiff_read({base,{level},{}},geometry);
    check(tiff.directory.width==9 && tiff.horizontal.offset_numerator!=0,"TIFF reduced fractional plan");
    compare_crop(mark,{tiff.directory.display_width(),tiff.directory.display_height(),
                     tiff.read_region.x,tiff.read_region.y},tiff.read_region.width,tiff.read_region.height);
    J2kMetadata metadata{}; metadata.width=17; metadata.height=13;
    metadata.origin_x=3; metadata.origin_y=5; metadata.resolution_count=4;
    const auto j2k=plan_j2k_read(metadata,geometry);
    check(j2k.reduction==1 && j2k.level_width==8 && j2k.level_height==6,"J2K nonzero-origin dimensions");
    compare_crop(mark,{j2k.level_width,j2k.level_height,j2k.reduced_region.x,j2k.reduced_region.y},
                 j2k.reduced_region.width,j2k.reduced_region.height);
    auto decoded=apply_watermark(black(j2k.reduced_region.width,j2k.reduced_region.height),mark,white,1,
        WatermarkGrid{j2k.level_width,j2k.level_height,j2k.reduced_region.x,j2k.reduced_region.y});
    const auto output=resize_image(std::move(decoded),j2k.window,4,3);
    check(output.width==4 && output.height==3,"Fractional window remains usable");
    for (const auto grid : {WatermarkGrid{0,1},WatermarkGrid{18,13},WatermarkGrid{17,14},
                           WatermarkGrid{1,1,1,0},WatermarkGrid{1,1,0,UINT32_MAX}}) {
        bool rejected=false;
        try { (void)apply_watermark(black(1,1),mark,white,1,grid); }
        catch (const WatermarkError&) { rejected=true; }
        check(rejected,"Invalid level accepted");
    }
    // uint32-limit canvas products, but only one prepared mask sample and output.
    const PreparedWatermark huge{std::make_shared<const RasterImage8>(one),UINT32_MAX,UINT32_MAX,UINT32_MAX-1,UINT32_MAX-1,1,1};
    check(apply_watermark(black(1,1),huge,white,1,WatermarkGrid{UINT32_MAX,UINT32_MAX,UINT32_MAX-1,UINT32_MAX-1}).pixels[0]==255,
          "Wide coordinate products");
}
}
int main() {
    try { run(); std::cout<<"Watermark grid tests passed\n"; }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
