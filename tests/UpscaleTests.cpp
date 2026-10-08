#include "image/AreaResize.h"
#include <iostream>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    try { action(); } catch (const sipi::ResizeError&) { return; }
    throw std::runtime_error("Expected ResizeError");
}
}
int main() {
    using namespace sipi;
    try {
        const PixelWindow full{{0,2,1},{0,2,1}};
        RasterImage8 gray{2,2,{0,80,160,240},{1,2,3},PixelLayout::gray};
        const auto enlarged = resize_image(gray, full, 4, 4);
        check(enlarged.pixels == std::vector<std::uint8_t>{
            0,20,60,80, 40,60,100,120, 120,140,180,200, 160,180,220,240}, "Bilinear centers");
        check(enlarged.icc_profile == gray.icc_profile && enlarged.layout == gray.layout, "Metadata");
        auto identity = resize_image(gray, full, 2, 2);
        check(identity.pixels == gray.pixels, "Identity");
        check(resize_image(gray, full, 1, 1).pixels == downsample_area(gray,1,1).pixels, "Area unchanged");
        RasterImage8 mixed{2,4,{0,80,0,80,160,240,160,240},{},PixelLayout::gray};
        check(resize_image(mixed,{{0,2,1},{0,4,1}},4,2).pixels ==
              std::vector<std::uint8_t>{0,20,60,80,160,180,220,240}, "Mixed area/linear");
        RasterImage8 alpha{2,1,{255,0,0,0,0,0,255,255},{},PixelLayout::rgba,0};
        const auto filtered = resize_image(alpha,{{0,2,1},{0,1,1}},4,1);
        check(filtered.pixels == std::vector<std::uint8_t>{0,0,0,0,0,0,255,64,0,0,255,191,0,0,255,255}, "Alpha fringe");
        check(filtered.srgb_intent == alpha.srgb_intent, "sRGB declaration");
        RasterImage8 ga{2,1,{255,0,80,255},{},PixelLayout::gray_alpha};
        check(resize_image(ga,{{0,2,1},{0,1,1}},4,1).pixels ==
              std::vector<std::uint8_t>{0,0,80,64,80,191,80,255}, "Gray alpha");
        RasterImage8 crop{3,1,{255,40,255},{},PixelLayout::gray};
        check(resize_image(crop,{{1,1,1},{0,1,1}},7,3).pixels ==
              std::vector<std::uint8_t>(21,40), "Crop edge must not leak neighbors");
        // Same center geometry represented by non-unit rational denominators.
        check(resize_image(gray,{{0,6,3},{0,10,5}},4,4).pixels == enlarged.pixels, "Rational window");
        rejects([&]{ (void)resize_image(gray,full,4,4,22); }); // 4 + 16 + 3 = 23 bytes.
        check(resize_image(gray,full,4,4,23).pixels == enlarged.pixels, "Exact budget");
        rejects([&]{ (void)resize_image(gray,full,0,4); });
        rejects([&]{ (void)resize_image(gray,{{0,1,0},{0,2,1}},4,4); });
        rejects([&]{ (void)resize_image(gray,{{2,1,1},{0,2,1}},4,4); });
        gray.pixels.pop_back();
        rejects([&]{ (void)resize_image(gray,full,4,4); });
        std::cout << "Upscaling, alpha, mixed axes and budgets passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
