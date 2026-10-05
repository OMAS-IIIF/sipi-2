#include "image/Bitonal.h"
#include "image/AlphaComposite.h"
#include <iostream>
#include <utility>

namespace {
using namespace sipi;
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
template<class Error, class F> void rejects(F f) {
    try { f(); } catch (const Error&) { return; }
    throw std::runtime_error("Expected rejection");
}
void run() {
    using enum BitonalMethod;
    RasterImage8 mid{2,2,{128,128,128,128},{},PixelLayout::gray};
    const auto dithered=convert_to_bitonal(mid,floyd_steinberg);
    check(dithered.pixels==std::vector<std::uint8_t>({255,0,0,255}),"2x2 diffusion");
    check(convert_to_bitonal(dithered,floyd_steinberg).pixels==dithered.pixels,"binary identity");
    for (const auto value : {0,255}) {
        RasterImage8 flat{1,8,std::vector<std::uint8_t>(8,value),{},PixelLayout::gray};
        check(convert_to_bitonal(flat,floyd_steinberg).pixels==flat.pixels,"endpoint/one-column identity");
    }
    // 4 input bytes plus two (width+2) rows of doubles: exactly 68 bytes.
    check(convert_to_bitonal(mid,floyd_steinberg,68).pixels==dithered.pixels,"exact budget");
    rejects<CompositeError>([&]{ (void)convert_to_bitonal(mid,floyd_steinberg,67); });
    rejects<BitonalError>([&]{ (void)convert_to_bitonal(mid,floyd_steinberg,1); });
    rejects<BitonalError>([&]{ (void)convert_to_bitonal(mid,static_cast<BitonalMethod>(99)); });

    RasterImage8 gray{6,1,{0,1,127,128,254,255},{},PixelLayout::gray};
    auto* storage=gray.pixels.data();
    const auto result=convert_to_bitonal(std::move(gray));
    check(result.pixels==std::vector<std::uint8_t>({0,0,0,255,255,255}),"threshold boundary");
    check(result.pixels.data()==storage,"in-place storage");
    RasterImage8 alpha{5,1,{0,0,0,127,0,128,127,255,128,255},{1,2,3},PixelLayout::gray_alpha};
    const auto opaque=convert_to_bitonal(alpha);
    check(opaque.layout==PixelLayout::gray &&
          opaque.pixels==std::vector<std::uint8_t>({255,255,0,0,255}),"white before threshold");
    check(opaque.icc_profile==alpha.icc_profile,"profile retained");
    check(convert_to_bitonal(opaque).pixels==opaque.pixels,"idempotence");
    alpha.icc_profile.clear(); alpha.srgb_intent=3;
    check(convert_to_bitonal(alpha).srgb_intent==3,"sRGB intent retained");
    rejects<CompositeError>([&]{ (void)convert_to_bitonal(alpha,BitonalMethod::threshold,9); });
    alpha.icc_profile={1};
    rejects<BitonalError>([&]{ (void)convert_to_bitonal(alpha); });
    alpha.icc_profile.clear(); alpha.srgb_intent=4;
    rejects<BitonalError>([&]{ (void)convert_to_bitonal(alpha); });
    rejects<BitonalError>([]{ (void)convert_to_bitonal({1,1,{0,0,0},{},PixelLayout::rgb}); });
    rejects<CompositeError>([]{ (void)convert_to_bitonal({0,1,{},{},PixelLayout::gray}); });
    rejects<CompositeError>([]{ (void)convert_to_bitonal({2,1,{0},{},PixelLayout::gray}); });
}
}
int main() {
    try { run(); std::cout<<"Bitonal tests passed\n"; }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
