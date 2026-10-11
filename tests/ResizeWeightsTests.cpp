#include "image/AreaResize.h"
#include <iostream>
#include <random>
#include <stdexcept>

/** Exercise cached/on-demand supports under the same exact pixel/ICC budget.
 * Fractional/mixed/identity cases and all four layouts must remain byte-identical;
 * existing area/upscale tests independently specify interpolation correctness. */
int main() {
    using namespace sipi;
    try {
        std::mt19937 random(20261012);
        unsigned checked = 0;
        for (auto layout : {PixelLayout::gray, PixelLayout::rgb,
                            PixelLayout::gray_alpha, PixelLayout::rgba}) {
            for (unsigned i = 0; i < 1000; ++i) {
                const unsigned sw = 2 + random()%31, sh = 2 + random()%29;
                const unsigned tw = 1 + random()%40, th = 1 + random()%37;
                RasterImage8 input{sw, sh, {}, {1,2,3}, layout};
                input.pixels.resize(sw*sh*input.channel_count());
                for (auto& byte : input.pixels) byte = random()%256;
                const std::uint32_t denominator = i%3 == 0 ? 1 : i%3 == 1 ? 3 : UINT32_MAX;
                const std::uint64_t ox = random()%denominator, oy = random()%denominator;
                PixelWindow window{{ox, std::uint64_t{sw}*denominator-ox, denominator},
                                   {oy, std::uint64_t{sh}*denominator-oy, denominator}};
                const auto exact_budget = input.pixels.size() + input.icc_profile.size() +
                                          tw*th*input.channel_count();
                const auto expected = resize_image(input, window, tw, th, exact_budget);
                const auto actual = resize_image(input, window, tw, th);
                if (actual.pixels != expected.pixels || actual.width != tw || actual.height != th ||
                    actual.layout != layout || actual.icc_profile != input.icc_profile)
                    throw std::runtime_error("Cached/on-demand axis mismatch");
                const bool identity = ox == 0 && oy == 0 && sw == tw && sh == th;
                if (!identity) {
                    bool rejected = false;
                    try { (void)resize_image(input, window, tw, th, exact_budget-1); }
                    catch (const ResizeError&) { rejected = true; }
                    if (!rejected) throw std::runtime_error("Pixel budget not enforced");
                }
                ++checked;
            }
        }
        std::cout << checked << " axis cache/fallback and budget cases passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
