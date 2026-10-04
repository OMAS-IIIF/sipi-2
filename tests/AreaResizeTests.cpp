#include "image/AreaResize.h"
#include <iostream>
#include <limits>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void rejects(F f) {
    try { f(); } catch (const sipi::ResizeError&) { return; }
    throw std::runtime_error("Expected ResizeError");
}
sipi::RgbImage8 pattern(std::uint32_t w, std::uint32_t h) {
    sipi::RgbImage8 image{w, h, std::vector<std::uint8_t>(w * h * 3), {1, 2, 3}};
    for (std::size_t i = 0; i < image.pixels.size(); ++i) image.pixels[i] = (i * 73 + i / 3 * 11) % 256;
    return image;
}
/** Independent small-image oracle: replicate source samples onto a common
 * integer grid, then count each target cell, rather than computing overlaps. */
std::vector<std::uint8_t> reference(const sipi::RgbImage8& in, std::uint32_t w, std::uint32_t h) {
    std::vector<std::uint8_t> out(w * h * 3);
    const std::uint64_t count = in.width * in.height;
    for (std::uint32_t y = 0; y < h; ++y)
        for (std::uint32_t x = 0; x < w; ++x)
            for (unsigned c = 0; c < 3; ++c) {
                std::uint64_t sum = 0;
                for (std::uint32_t j = 0; j < in.height; ++j)
                    for (std::uint32_t i = 0; i < in.width; ++i) {
                        const auto sx = (x * in.width + i) / w;
                        const auto sy = (y * in.height + j) / h;
                        sum += in.pixels[3 * (sy * in.width + sx) + c];
                    }
                out[3 * (y * w + x) + c] = static_cast<std::uint8_t>((2 * sum + count) / (2 * count));
            }
    return out;
}
}
int main() {
    try {
        for (std::uint32_t sw = 1; sw <= 9; ++sw)
            for (std::uint32_t sh = 1; sh <= 9; ++sh)
                for (std::uint32_t w = 1; w <= sw; ++w)
                    for (std::uint32_t h = 1; h <= sh; ++h) {
                        const auto input = pattern(sw, sh);
                        const auto out = sipi::downsample_area(input, w, h);
                        check(out.width == w && out.height == h && out.pixels == reference(input, w, h), "Area oracle mismatch");
                        check(out.icc_profile == input.icc_profile, "Profile changed");
                    }
        // Independent common-grid oracle for fractional windows. Each rational
        // cell has equal area; counting cells avoids the production overlap code.
        for (std::uint32_t dx : {2u, 3u, 5u})
            for (std::uint32_t dy : {2u, 3u})
                for (std::uint32_t ox = 0; ox < dx; ++ox)
                    for (std::uint32_t oy = 0; oy < dy; ++oy)
                        for (std::uint32_t w = 1; w <= 3; ++w)
                            for (std::uint32_t h = 1; h <= 2; ++h) {
                                const auto input = pattern(5, 4);
                                const std::uint32_t ex = 4 * dx + 1, ey = 3 * dy + 1;
                                const sipi::PixelWindow window{{ox, ex, dx}, {oy, ey, dy}};
                                const auto out = sipi::downsample_area(input, window, w, h);
                                check(out.icc_profile == input.icc_profile, "Fractional ICC preservation");
                                for (std::uint32_t y = 0; y < h; ++y)
                                    for (std::uint32_t x = 0; x < w; ++x)
                                        for (unsigned c = 0; c < 3; ++c) {
                                            std::uint64_t sum = 0;
                                            for (std::uint32_t j = 0; j < ey; ++j)
                                                for (std::uint32_t i = 0; i < ex; ++i) {
                                                    const auto sx = (ox*w + x*ex + i) / (dx*w);
                                                    const auto sy = (oy*h + y*ey + j) / (dy*h);
                                                    sum += input.pixels[3*(sy*5+sx)+c];
                                                }
                                            const auto expected = (2*sum + ex*ey) / (2*ex*ey);
                                            const auto actual = out.pixels[3*(y*w+x)+c];
                                            // Floating point accumulation can differ by one
                                            // at a mathematical half-integer quantization tie.
                                            const bool tie = 2 * (sum % (ex*ey)) == ex*ey;
                                            check(actual == expected || (tie && actual + 1 == expected),
                                                  "Fractional window oracle mismatch");
                                        }
                            }
        const auto fractional = pattern(3, 2);
        const sipi::PixelWindow window{{1, 4, 2}, {0, 2, 1}};
        check(sipi::downsample_area(fractional, window, 2, 1, 27).pixels.size() == 6,
              "Fractional exact budget");
        rejects([&] { (void)sipi::downsample_area(fractional, window, 2, 1, 26); });
        for (const sipi::PixelWindow invalid : {
             sipi::PixelWindow{{0, 1, 0}, {0, 2, 1}}, {{0, 0, 1}, {0, 2, 1}},
             {{1, 6, 2}, {0, 2, 1}}, {{UINT64_MAX, 2, 2}, {0, 2, 1}},
             {{0, UINT64_MAX, 2}, {0, 2, 1}}, {{0, 1, 2}, {0, 2, 1}}})
            rejects([&] { (void)sipi::downsample_area(fractional, invalid, 1, 1); });
        // A direct offset/extent * output-index calculation would overflow 64 bits.
        const auto denominator = std::numeric_limits<std::uint32_t>::max();
        const auto large_fraction = sipi::downsample_area(
            {100001, 1, std::vector<std::uint8_t>(300003, 173), {}},
            sipi::PixelWindow{{denominator/2, std::uint64_t{100000}*denominator, denominator}, {0,1,1}},
            100000, 1);
        check(large_fraction.pixels == std::vector<std::uint8_t>(300000, 173),
              "Large rational boundary products");
        const auto shifted = sipi::downsample_area(
            {3, 1, {0,0,0, 100,100,100, 200,200,200}, {}},
            sipi::PixelWindow{{1,4,2}, {0,1,1}}, 2, 1);
        check(shifted.pixels == std::vector<std::uint8_t>({50,50,50,150,150,150}),
              "Fractional edges must not resize the whole enclosure");
        auto identity = pattern(3, 2);
        const auto* storage = identity.pixels.data();
        auto same = sipi::downsample_area(std::move(identity), 3, 2, 21);
        check(same.pixels.data() == storage, "Identity copied storage");
        check(sipi::downsample_area(same, 1, 1, 24).pixels.size() == 3, "Exact memory budget rejected");
        rejects([&] { (void)sipi::downsample_area(same, 1, 1, 23); });
        rejects([&] { (void)sipi::downsample_area(same, 3, 2, 20); });
        rejects([&] { (void)sipi::downsample_area(same, 0, 1); });
        rejects([&] { (void)sipi::downsample_area(same, 4, 1); });
        rejects([&] { (void)sipi::downsample_area(same, 1, 3); });
        rejects([&] { (void)sipi::downsample_area({3, 2, {1}, {}}, 1, 1); });
        rejects([&] { (void)sipi::downsample_area({0, 2, {}, {}}, 1, 1); });
        rejects([&] { (void)sipi::downsample_area({UINT32_MAX, UINT32_MAX, {}, {}}, 1, 1); });
        const auto average = sipi::downsample_area({2, 1, {0, 0, 0, 255, 255, 255}, {}}, 1, 1);
        check(average.pixels == std::vector<std::uint8_t>({128, 128, 128}), "Stored-value half-up semantics");
        const auto constant = sipi::downsample_area({7, 5, std::vector<std::uint8_t>(105, 255), {}}, 3, 2);
        check(constant.pixels == std::vector<std::uint8_t>(18, 255), "Constant changed");
        std::cout << "Area resize oracle, edges, ownership and limits passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
