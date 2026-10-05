#include "image/GrayConversion.h"
#include <lcms2.h>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>

namespace {
using namespace sipi;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    try { f(); } catch (const ColorConversionError&) { return; }
    throw std::runtime_error("Expected color conversion failure");
}
/** Independent linear-RGB input: neutral 128 should become ~186, not sRGB's ~127. */
std::vector<std::uint8_t> linear_profile() {
    const cmsCIExyY white{.3127,.3290,1};
    const cmsCIExyYTRIPLE primaries{{.64,.33,1},{.30,.60,1},{.15,.06,1}};
    auto* curve=cmsBuildGamma(nullptr,1);
    cmsToneCurve* curves[]{curve,curve,curve};
    auto profile=cmsCreateRGBProfile(&white,&primaries,curves);
    check(profile,"linear profile");
    cmsUInt32Number size=0; check(cmsSaveProfileToMem(profile,nullptr,&size),"profile size");
    std::vector<std::uint8_t> bytes(size);
    check(cmsSaveProfileToMem(profile,bytes.data(),&size),"profile data");
    cmsCloseProfile(profile); cmsFreeToneCurve(curve);
    return bytes;
}
void run(const std::filesystem::path& directory) {
    RasterImage8 source{3,1,{0,0,0,128,128,128,255,255,255},{},PixelLayout::rgb};
    const auto untagged=convert_to_gray(source);
    source.srgb_intent=3;
    const auto tagged=convert_to_gray(source);
    check(untagged.pixels==tagged.pixels && untagged.icc_profile==tagged.icc_profile,"sRGB fallback mismatch");
    check(tagged.layout==PixelLayout::gray && !tagged.srgb_intent,"gray target state");
    check(tagged.pixels[0]==0 && tagged.pixels[2]==255 && std::abs(int(tagged.pixels[1])-127)<=1,"neutral sRGB conversion");
    auto profile=cmsOpenProfileFromMem(tagged.icc_profile.data(),static_cast<cmsUInt32Number>(tagged.icc_profile.size()));
    check(profile && cmsGetColorSpace(profile)==cmsSigGrayData,"target GRAY ICC");
    auto* curve=static_cast<cmsToneCurve*>(cmsReadTag(profile,cmsSigGrayTRCTag));
    check(curve && std::abs(cmsEvalToneCurveFloat(curve,.5f)-std::pow(.5,2.2))<.00001,"target gamma 2.2");
    const auto* white=static_cast<cmsCIEXYZ*>(cmsReadTag(profile,cmsSigMediaWhitePointTag));
    check(white && std::abs(white->X-.9642)<.0001 && std::abs(white->Y-1)<.0001 && std::abs(white->Z-.8249)<.0001,"target D50");
    check(cmsGetHeaderRenderingIntent(profile)==INTENT_RELATIVE_COLORIMETRIC,"target intent");
    cmsCloseProfile(profile);
    auto linear=source; linear.srgb_intent.reset(); linear.icc_profile=linear_profile();
    const auto converted=convert_to_gray(linear);
    check(std::abs(int(converted.pixels[1])-186)<=1,"embedded linear profile ignored");
    RasterImage8 alpha{3,1,{128,128,128,0,128,128,128,123,128,128,128,255},{},PixelLayout::rgba};
    const auto transparent=convert_to_gray(alpha);
    check(transparent.layout==PixelLayout::gray_alpha && transparent.pixels[1]==0 && transparent.pixels[3]==123 && transparent.pixels[5]==255,"alpha changed");
    check(transparent.pixels[0]==tagged.pixels[1] && transparent.pixels[2]==tagged.pixels[1],"alpha affected color");
    auto original=transparent;
    const auto* pixels=original.pixels.data(); const auto* icc=original.icc_profile.data();
    auto unchanged=convert_to_gray(std::move(original));
    check(unchanged.pixels.data()==pixels && unchanged.icc_profile.data()==icc,"native gray was converted/copied");
    auto invalid=source; invalid.srgb_intent.reset(); invalid.icc_profile={1,2,3};
    rejects([&]{ (void)convert_to_gray(invalid); });
    invalid.icc_profile=tagged.icc_profile;
    rejects([&]{ (void)convert_to_gray(invalid); }); // GRAY profile on RGB is not a fallback.
    auto conflict=linear; conflict.srgb_intent=0;
    rejects([&]{ (void)convert_to_gray(conflict); });
    const auto budget=source.pixels.size()+tagged.pixels.size()+tagged.icc_profile.size();
    (void)convert_to_gray(source,budget);
    rejects([&]{ (void)convert_to_gray(source,budget-1); });
    std::vector<std::future<void>> tasks;
    for (int i=0;i<8;++i) tasks.push_back(std::async(std::launch::async,[&] {
        auto result=convert_to_gray(source);
        check(result.icc_profile==tagged.icc_profile && result.pixels==tagged.pixels,"parallel deterministic conversion");
    }));
    for (auto& task:tasks) task.get();
    std::filesystem::create_directories(directory);
    std::ofstream file(directory/"linear-rgb.icc",std::ios::binary);
    file.write(reinterpret_cast<const char*>(linear.icc_profile.data()),linear.icc_profile.size());
    check(bool(file),"fixture write");
}
}
int main(int argc,char** argv) {
    if (argc!=2) return 2;
    try { run(argv[1]); } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
    std::cout<<"Profile conversion, D50/gamma, alpha, limits and determinism passed\n";
}
