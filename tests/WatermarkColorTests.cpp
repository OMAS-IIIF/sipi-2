#include "image/WatermarkColor.h"
#include "image/Watermark.h"
#include "image/SrgbProfiles.h"
#include <lcms2.h>
#include <cmath>
#include <future>
#include <iostream>

namespace {
using namespace sipi;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    try { action(); } catch (const ColorConversionError&) { return; }
    throw std::runtime_error("Expected watermark color rejection");
}
/** Known linear RGB/gray targets provide a numeric oracle independent of the converter. */
std::vector<std::uint8_t> linear_profile(bool gray) {
    auto* curve=cmsBuildGamma(nullptr,1);
    const cmsCIExyY white{.3127,.3290,1};
    const cmsCIExyYTRIPLE primaries{{.64,.33,1},{.30,.60,1},{.15,.06,1}};
    cmsToneCurve* curves[]{curve,curve,curve};
    auto profile=gray ? cmsCreateGrayProfile(cmsD50_xyY(),curve) : cmsCreateRGBProfile(&white,&primaries,curves);
    check(profile,"profile allocation");
    cmsUInt32Number size=0;
    check(cmsSaveProfileToMem(profile,nullptr,&size),"profile size");
    std::vector<std::uint8_t> bytes(size);
    check(cmsSaveProfileToMem(profile,bytes.data(),&size),"profile serialization");
    cmsCloseProfile(profile); cmsFreeToneCurve(curve);
    return bytes;
}
void run() {
    const std::array<std::uint8_t,3> neutral{128,128,128}, red{255,0,0};
    RasterImage8 rgb{1,1,{4,5,6},{},PixelLayout::rgb};
    check(watermark_color(rgb,red)==std::vector<std::uint8_t>({255,0,0}),"untagged sRGB identity");
    for (unsigned intent=0;intent<4;++intent) {
        rgb.srgb_intent=intent;
        check(watermark_color(rgb,neutral)==std::vector<std::uint8_t>({128,128,128}),"explicit RGB sRGB identity");
    }
    rgb.srgb_intent.reset(); rgb.icc_profile=linear_profile(false);
    const auto linear=watermark_color(rgb,neutral);
    const int expected=std::lround(255*std::pow((128/255.0+.055)/1.055,2.4));
    for (auto c:linear) check(std::abs(int(c)-expected)<=1,"linear RGB transfer");
    auto rgba=rgb; rgba.layout=PixelLayout::rgba; rgba.pixels.push_back(0);
    check(watermark_color(rgba,neutral)==linear,"alpha influenced color conversion");
    const auto original=rgba;
    auto mask=prepare_watermark({1,1,{255},{},PixelLayout::gray},1,1,1);
    auto marked=apply_watermark(rgba,mask,linear,1);
    check(marked.pixels==std::vector<std::uint8_t>({linear[0],linear[1],linear[2],255}),"converted color composition");
    check(rgba.pixels==original.pixels && rgba.icc_profile==original.icc_profile &&
          marked.icc_profile==original.icc_profile,"destination mutated or retagged");
    RasterImage8 gray{1,1,{17},{},PixelLayout::gray};
    auto gray_linear=gray; gray_linear.icc_profile=linear_profile(true);
    check(std::abs(int(watermark_color(gray_linear,neutral)[0])-expected)<=1,"linear gray transfer");
    check(std::abs(int(watermark_color(gray,neutral)[0])-127)<=1,"gray gamma 2.2 assumption");
    // Compare the fallback against the existing RGB-to-gray pipeline for chromatic input.
    RasterImage8 swatch{1,1,{255,0,0},{},PixelLayout::rgb};
    const auto converted=convert_to_gray(swatch);
    check(watermark_color(gray,red)==converted.pixels,"gray policy diverged");
    gray.icc_profile=converted.icc_profile;
    check(watermark_color(gray,red)==converted.pixels,"embedded gamma gray policy");
    gray.icc_profile.clear(); gray.srgb_intent=2;
    check(std::abs(int(watermark_color(gray,neutral)[0])-128)<=1,"explicit gray sRGB transfer");
    auto ga=gray; ga.layout=PixelLayout::gray_alpha; ga.pixels.push_back(0);
    check(watermark_color(ga,red)==watermark_color(gray,red),"gray alpha conversion");
    // Embedded sRGB still validates and transforms; it is not bypassed by the identity path.
    rgb.icc_profile=srgb_profile(PixelLayout::rgb,0);
    check(watermark_color(rgb,red)==std::vector<std::uint8_t>({255,0,0}),"embedded sRGB");
    const auto budget=rgb.pixels.size()+rgb.icc_profile.size();
    (void)watermark_color(rgb,red,budget);
    rejects([&]{(void)watermark_color(rgb,red,budget-1);});
    auto bad=rgb; bad.icc_profile={1,2,3};
    rejects([&]{(void)watermark_color(bad,red);});
    bad.icc_profile=linear_profile(true);
    rejects([&]{(void)watermark_color(bad,red);});
    bad=gray; bad.srgb_intent.reset(); bad.icc_profile=rgb.icc_profile;
    rejects([&]{(void)watermark_color(bad,red);});
    bad=rgb; bad.srgb_intent=0;
    rejects([&]{(void)watermark_color(bad,red);});
    bad.icc_profile.clear(); bad.srgb_intent=4;
    rejects([&]{(void)watermark_color(bad,red);});
    bad=rgb; bad.pixels.clear();
    rejects([&]{(void)watermark_color(bad,red);});
    bad=rgb; bad.icc_profile.resize(16*1024*1024+1);
    rejects([&]{(void)watermark_color(bad,red);});
    for (const std::string signature : {"link","abst","nmcl"}) {
        bad=rgb;
        std::copy(signature.begin(),signature.end(),bad.icc_profile.begin()+12);
        rejects([&]{(void)watermark_color(bad,red);});
    }
    std::vector<std::future<std::vector<std::uint8_t>>> jobs;
    for (unsigned i=0;i<8;++i) jobs.push_back(std::async(std::launch::async,[&]{return watermark_color(rgba,neutral);}));
    for (auto& job:jobs) check(job.get()==linear,"parallel conversion nondeterministic");
}
}
int main() {
    try {run(); std::cout<<"Watermark color tests passed\n";}
    catch(const std::exception& e) {std::cerr<<e.what()<<'\n'; return 1;}
}
