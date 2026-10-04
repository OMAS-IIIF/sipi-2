#include "imgformats/TiffReadPlan.h"

#include <iostream>
#include <limits>

namespace {
void check(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
template<class F> void rejects(F action) {
    try { action(); } catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Expected invalid_argument");
}
sipi::TiffDirectory directory(std::uint32_t w, std::uint32_t h, std::uint64_t offset) {
    sipi::TiffDirectory result;
    result.width = w;
    result.height = h;
    result.offset = offset;
    return result;
}
void check_axis(const sipi::PixelInterval& axis, std::uint32_t origin,
                std::uint32_t extent, std::uint32_t base, std::uint32_t level,
                std::uint32_t read_origin, std::uint32_t read_length) {
    using W = std::uint64_t;
    check(axis.denominator == base, "Wrong denominator");
    check(W{read_origin} * base + axis.offset_numerator == W{origin} * level,
          "Requested origin shifted");
    check(axis.extent_numerator == W{extent} * level, "Requested extent changed");
    check(axis.offset_numerator < base, "Nonminimal left enclosure");
    const W right = axis.offset_numerator + axis.extent_numerator;
    check(right <= W{read_length} * base && right > W{read_length - 1} * base,
          "Nonminimal right enclosure");
}
}
int main() {
    try {
        sipi::TiffMetadata metadata{directory(1001, 751, 10),
            {directory(251, 188, 30), directory(501, 376, 20)}, {}};
        auto plan = sipi::plan_tiff_read(metadata, {{0, 0, 1001, 751}, 250, 187});
        check(plan.directory.offset == 30, "Smallest sufficient unordered level");
        plan = sipi::plan_tiff_read(metadata, {{0, 0, 1001, 751}, 251, 188});
        check(plan.directory.offset == 30, "Exact threshold rejected");
        plan = sipi::plan_tiff_read(metadata, {{0, 0, 1001, 751}, 252, 188});
        check(plan.directory.offset == 20, "Width threshold ignored");
        plan = sipi::plan_tiff_read(metadata, {{0, 0, 1001, 751}, 251, 189});
        check(plan.directory.offset == 20, "Height threshold ignored");
        plan = sipi::plan_tiff_read(metadata, {{3, 5, 503, 377}, 125, 94});
        check(plan.directory.offset == 30 && plan.read_region.x == 0 &&
              plan.read_region.y == 1 && plan.read_region.width == 127 &&
              plan.read_region.height == 95, "Fractional rectangle enclosure");
        check_axis(plan.horizontal, 3, 503, 1001, 251, 0, 127);
        check_axis(plan.vertical, 5, 377, 751, 188, 1, 95);
        // The enclosing read is 127 pixels wide, but the exact crop is <127.
        plan = sipi::plan_tiff_read(metadata, {{3, 5, 503, 377}, 127, 94});
        check(plan.directory.offset == 20, "Rounded enclosure used for selection");
        plan = sipi::plan_tiff_read(metadata, {{17, 19, 3, 2}, 3, 2});
        check(plan.directory.offset == 10 && plan.read_region.x == 17 &&
              plan.horizontal.offset_numerator == 0, "Full resolution fallback");
        plan = sipi::plan_tiff_read(metadata, {{0, 0, 1001, 751}, 2000, 1500});
        check(plan.directory.offset == 10, "Upscaling must retain full resolution");
        metadata.levels.clear();
        plan = sipi::plan_tiff_read(metadata, {{1, 2, 100, 100}, 1, 1});
        check(plan.directory.offset == 10, "Nonpyramidal fallback");

        // Exhaustive small rectangles: independent enclosure/selection invariants,
        // including odd sizes, edge clipping already resolved, and one-pixel axes.
        for (std::uint32_t base = 2; base <= 15; ++base)
            for (std::uint32_t level = 1; level < base; ++level) {
                sipi::TiffMetadata m{directory(base, base, 1), {directory(level, level, 2)}, {}};
                for (std::uint32_t x = 0; x < base; ++x)
                    for (std::uint32_t w = 1; w <= base - x; ++w)
                        for (std::uint32_t out = 1; out <= w; ++out) {
                            const auto p = sipi::plan_tiff_read(m, {{x, x, w, w}, out, out});
                            // Long double reference is exact enough for these small values.
                            const bool sufficient = static_cast<long double>(w) * level / base >= out;
                            check(p.directory.offset == (sufficient ? 2 : 1), "Selection oracle");
                            const auto chosen = sufficient ? level : base;
                            check_axis(p.horizontal, x, w, base, chosen,
                                       p.read_region.x, p.read_region.width);
                            check_axis(p.vertical, x, w, base, chosen,
                                       p.read_region.y, p.read_region.height);
                        }
            }
        // Orientation normalization swaps dimensions for transposed orientations;
        // this geometric plan does not promise decoder support for any orientation.
        for (unsigned orientation = 1; orientation <= 8; ++orientation) {
            sipi::TiffMetadata m{directory(100, 60, 1), {directory(50, 30, 2)}, {}};
            m.image.orientation = m.levels[0].orientation = static_cast<sipi::TiffOrientation>(orientation);
            const auto w = m.image.display_width(), h = m.image.display_height();
            const auto p = sipi::plan_tiff_read(m, {{0, 0, w, h}, w / 2, h / 2});
            check(p.read_region.width == w / 2 && p.read_region.height == h / 2,
                  "Normalized orientation dimensions");
        }
        // Near uint32 limits: no pixels are allocated and products still fit uint64.
        constexpr auto max = std::numeric_limits<std::uint32_t>::max();
        sipi::TiffMetadata large{directory(max, max, 1), {directory(max - 1, max - 1, 2)}, {}};
        plan = sipi::plan_tiff_read(large, {{0, 0, max, max}, max - 1, max - 1});
        check(plan.directory.offset == 2 && plan.read_region.width == max - 1, "Large products");
        plan = sipi::plan_tiff_read(large, {{1, 1, max - 1, max - 1}, max - 2, max - 2});
        check_axis(plan.horizontal, 1, max - 1, max, max - 1,
                   plan.read_region.x, plan.read_region.width);
        rejects([&] { (void)sipi::plan_tiff_read(large, {{max, 0, 2, 1}, 1, 1}); });
        rejects([&] { (void)sipi::plan_tiff_read(metadata, {{0, 0, 0, 1}, 1, 1}); });
        rejects([&] { (void)sipi::plan_tiff_read(metadata, {{0, 0, 1, 1}, 0, 1}); });
        metadata.image.width = 0;
        rejects([&] { (void)sipi::plan_tiff_read(metadata, {{0, 0, 1, 1}, 1, 1}); });
        metadata.image = directory(100, 100, 1);
        metadata.levels = {directory(101, 50, 2)};
        rejects([&] { (void)sipi::plan_tiff_read(metadata, {{0, 0, 1, 1}, 1, 1}); });
        metadata.levels = {directory(50, 50, 2)};
        metadata.levels[0].orientation = sipi::TiffOrientation::bottom_left;
        rejects([&] { (void)sipi::plan_tiff_read(metadata, {{0, 0, 1, 1}, 1, 1}); });
        metadata.image.orientation = static_cast<sipi::TiffOrientation>(9);
        rejects([&] { (void)sipi::plan_tiff_read(metadata, {{0, 0, 1, 1}, 1, 1}); });
        std::cout << "TIFF read plan tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
