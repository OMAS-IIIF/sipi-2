#include "image/ImageTransform.h"
#include "image/AlphaComposite.h"
#include "image/AreaResize.h"
#include <cmath>
#include <iostream>
#include <limits>

namespace {
using namespace sipi;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    try { f(); } catch (const TransformError&) { return; }
    throw std::runtime_error("Expected transform rejection");
}
void run() {
    // Analytic 1x1 rotation: four equal bilinear weights of 1 - sqrt(1/2).
    for (const auto layout : {PixelLayout::gray,PixelLayout::rgb,PixelLayout::rgba,PixelLayout::gray_alpha}) {
        RasterImage8 source{1,1,{}, {},layout,2};
        source.pixels = layout == PixelLayout::gray ? std::vector<std::uint8_t>{100} :
            layout == PixelLayout::rgb ? std::vector<std::uint8_t>{255,0,0} :
            layout == PixelLayout::rgba ? std::vector<std::uint8_t>{255,0,0,128} :
                                         std::vector<std::uint8_t>{100,128};
        auto result = transform_image(source,false,45);
        const auto channels = result.channel_count();
        check(result.width == 2 && result.height == 2, "45-degree bounds");
        check(result.srgb_intent == source.srgb_intent, "sRGB lost");
        for (unsigned i=0;i<4;++i) {
            check(result.pixels[i*channels] == source.pixels[0], "darkened edge color");
            check(result.pixels[i*channels+channels-1] == (source.channel_count()%2 ? 75 : 37), "analytic alpha");
        }
        for (unsigned turn=0;turn<4;++turn) {
            auto actual=transform_image(source,true,turn*90);
            auto exact=transform_orthogonal(source,true,static_cast<QuarterTurn>(turn));
            check(actual.pixels==exact.pixels && actual.layout==exact.layout,"exact path changed");
        }
        const auto needed=source.pixels.size()+result.pixels.size();
        (void)transform_image(source,false,45,needed);
        rejects([&]{ (void)transform_image(source,false,45,needed-1); });
    }
    RasterImage8 colored{2,1,{255,0,0,255, 0,0,255,0},{},PixelLayout::rgba};
    auto rotated=transform_image(colored,false,31);
    for (std::size_t i=0;i<rotated.pixels.size();i+=4) {
        check(rotated.pixels[i+1]==0 && rotated.pixels[i+2]==0,"transparent blue contaminated result");
        check(rotated.pixels[i]==(rotated.pixels[i+3] ? 255 : 0),"straight alpha or hidden black");
    }
    auto reflected=transform_image(colored,true,31);
    auto mirrored=transform_orthogonal(colored,true,QuarterTurn::none);
    check(reflected.pixels==transform_image(mirrored,false,31).pixels,"mirror order");
    RasterImage8 gray{2,1,{100,0,200,255},{9,8,7},PixelLayout::gray_alpha};
    auto reduced=downsample_area(gray,1,1);
    check(reduced.pixels==std::vector<std::uint8_t>({200,128}),"gray alpha reduction");
    auto white=composite_on_white(reduced);
    check(white.layout==PixelLayout::gray && white.pixels[0]==227 && white.icc_profile==gray.icc_profile,"gray ICC/white composite");
    auto preserved=transform_image(gray,false,17);
    check(preserved.layout==PixelLayout::gray_alpha && preserved.icc_profile==gray.icc_profile,"GRAY ICC changed");
    for (double angle : {-1.,361.,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
        rejects([&]{ (void)rotated_size(2,3,angle); });
    rejects([&]{ (void)rotated_size(0,3,45); });
    rejects([&]{ (void)rotated_size(UINT32_MAX,UINT32_MAX,45); });
    for (double angle : {0.,45.}) {
        auto invalid=colored; invalid.pixels.pop_back();
        rejects([&]{ (void)transform_image(invalid,false,angle); });
    }
    check(rotated_size(3,2,90).width==2 && rotated_size(3,2,360).height==2,"integer geometry");
    check(rotated_size(3,2,89.999999).width==3,"near angle was snapped");
}
}
int main() {
    try { run(); } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
    std::cout<<"Bilinear rotation, alpha, exact paths, color and bounds passed\n";
}
