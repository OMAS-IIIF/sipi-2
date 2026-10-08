#include "image/Watermark.h"
#include <array>
#include <iostream>
#include <limits>

namespace {
using namespace sipi;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    try { f(); } catch (const WatermarkError&) { return; }
    throw std::runtime_error("Expected watermark rejection");
}
RasterImage8 mask(std::uint32_t w, std::uint32_t h, std::vector<std::uint8_t> bytes) {
    return {w,h,std::move(bytes),{},PixelLayout::gray};
}
void run() {
    const std::array<std::uint8_t,1> white{255};
    const std::array<std::uint8_t,3> red{255,0,0};
    auto mark=prepare_watermark(mask(2,1,{0,255}),8,6,0.5);
    check(mark.x==2 && mark.y==2 && mark.width==4 && mark.height==2,"center/relative width");
    check(mark.mask->pixels==std::vector<std::uint8_t>({0,255}),"original mask retained");
    auto enlarged=apply_watermark(mask(4,2,std::vector<std::uint8_t>(8)),mark,white,1,2,2);
    check(enlarged.pixels==std::vector<std::uint8_t>({0,64,191,255,0,64,191,255}),"bilinear coverage");
    auto tall=prepare_watermark(mask(1,4,{255,255,255,255}),10,4,1);
    check(tall.width==1 && tall.height==4 && tall.x==4,"height fit/odd centering");
    auto reduced=prepare_watermark(mask(2,2,{0,255,255,0}),1,1,1);
    check(apply_watermark(mask(1,1,{0}),reduced,white,1).pixels[0]==128,"binary mask area reduction");
    auto tiny=prepare_watermark(mask(1,1,{255}),8,6,0.001);
    check(tiny.width==1 && tiny.height==1,"minimum extent");

    // Independently render native-resolution crops: they must match slices of
    // the full canvas, including tiles that do not intersect the watermark.
    auto canvas=mask(8,6,std::vector<std::uint8_t>(48,40));
    const auto whole=apply_watermark(canvas,mark,white,0.6);
    for (unsigned y=0;y<6;y+=2) for (unsigned x=0;x<8;x+=2) {
        auto crop=apply_watermark(mask(2,2,{40,40,40,40}),mark,white,0.6,x,y);
        for (unsigned dy=0;dy<2;++dy) for (unsigned dx=0;dx<2;++dx)
            check(crop.pixels[dy*2+dx]==whole.pixels[(y+dy)*8+x+dx],"crop anchoring mismatch");
    }
    check(whole.pixels[0]==40 && whole.pixels[2*8+2]==40,"outside/transparent mask changed");
    check(apply_watermark(canvas,mark,white,0).pixels==canvas.pixels,"zero opacity changed pixels");
    auto opaque=prepare_watermark(mask(1,1,{255}),1,1,1);
    auto half=prepare_watermark(mask(1,1,{128}),1,1,1);
    check(apply_watermark(mask(1,1,{0}),half,white,0.5).pixels[0]==64,"mask times opacity");
    RasterImage8 rgb{1,1,{0,0,255},{1,2,3},PixelLayout::rgb};
    auto blended=apply_watermark(rgb,opaque,red,0.5);
    check(blended.pixels==std::vector<std::uint8_t>({128,0,128}),"RGB blend");
    check(blended.icc_profile==rgb.icc_profile && blended.layout==rgb.layout,"profile/layout lost");
    RasterImage8 rgba{1,1,{0,0,255,0},{},PixelLayout::rgba,2};
    auto visible=apply_watermark(rgba,opaque,red,0.5);
    check(visible.pixels==std::vector<std::uint8_t>({255,0,0,128}),"hidden RGB contaminated blend");
    check(visible.srgb_intent==2,"sRGB declaration lost");
    rgba.pixels[3]=128;
    check(apply_watermark(rgba,opaque,red,0.5).pixels==std::vector<std::uint8_t>({170,0,85,192}),"straight source-over");
    auto ga=mask(1,1,{0,128}); ga.layout=PixelLayout::gray_alpha;
    check(apply_watermark(ga,opaque,white,0.5).pixels==std::vector<std::uint8_t>({170,192}),"gray alpha blend");
    (void)apply_watermark(rgb,opaque,red,1,0,0,7);
    rejects([&]{(void)apply_watermark(rgb,opaque,red,1,0,0,6);});
    rejects([&]{(void)apply_watermark(rgb,opaque,white,1);});
    rejects([&]{(void)apply_watermark(rgb,opaque,red,-0.1);});
    rejects([&]{(void)apply_watermark(rgb,opaque,red,std::numeric_limits<double>::quiet_NaN());});
    rejects([&]{(void)apply_watermark(rgb,opaque,red,1,1,0);});
    auto broken=opaque; broken.x=std::numeric_limits<std::uint32_t>::max();
    rejects([&]{(void)apply_watermark(rgb,broken,red,1);});
    rgb.pixels.pop_back();
    rejects([&]{(void)apply_watermark(rgb,opaque,red,1);});
    for (double f : {0.0,-1.0,1.1,std::numeric_limits<double>::infinity()})
        rejects([&]{(void)prepare_watermark(mask(1,1,{255}),1,1,f);});
    rejects([&]{(void)prepare_watermark(mask(0,1,{}),1,1,1);});
    rejects([&]{(void)prepare_watermark(mask(1,1,{}),1,1,1);});
    auto tagged=mask(1,1,{255}); tagged.srgb_intent=0;
    rejects([&]{(void)prepare_watermark(tagged,1,1,1);});
    tagged.srgb_intent.reset(); tagged.icc_profile={1};
    rejects([&]{(void)prepare_watermark(tagged,1,1,1);});
    rejects([&]{(void)prepare_watermark(mask(1,1,{255}),10,10,1,0);});
    auto huge=prepare_watermark(mask(1,1,{255}),UINT32_MAX,UINT32_MAX,1,1);
    check(huge.mask->pixels.size()==1 && huge.width==UINT32_MAX,"canvas-sized mask allocated");
    check(apply_watermark(mask(1,1,{0}),huge,white,1,UINT32_MAX-1,UINT32_MAX-1,2).pixels[0]==255,
          "huge native crop with two-byte working budget");
    huge.width=0;
    rejects([&]{(void)apply_watermark(mask(1,1,{0}),huge,white,1);});
}
}
int main() {
    try { run(); std::cout<<"Watermark tests passed\n"; }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
