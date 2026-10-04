#include "TiffReadPlan.h"

namespace sipi {
namespace {
using Wide = std::uint64_t;

void validate_directory(const TiffDirectory& directory) {
    const auto orientation = static_cast<unsigned>(directory.orientation);
    if (!directory.width || !directory.height || orientation < 1 || orientation > 8)
        throw std::invalid_argument("Invalid TIFF directory geometry");
}

struct AxisPlan {
    std::uint32_t origin, length;
    PixelInterval window;
};

/** Integer products fit uint64_t: validated edges and dimensions are uint32_t.
 * Quotient/remainder ceiling avoids adding a denominator near UINT64_MAX. */
AxisPlan plan_axis(std::uint32_t origin, std::uint32_t extent,
                   std::uint32_t base, std::uint32_t level) {
    const Wide left = Wide{origin} * level;
    const Wide right = (Wide{origin} + extent) * level;
    const Wide first = left / base;
    const Wide end = right / base + (right % base != 0);
    return {static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(end - first),
            {left % base, Wide{extent} * level, base}};
}
} // namespace

TiffReadPlan plan_tiff_read(const TiffMetadata& metadata,
                           const iiif::ImageGeometry& geometry) {
    const auto& base = metadata.image;
    validate_directory(base);
    const auto width = base.display_width(), height = base.display_height();
    const auto& crop = geometry.region;
    if (!crop.width || !crop.height || !geometry.output_width || !geometry.output_height ||
        Wide{crop.x} + crop.width > width || Wide{crop.y} + crop.height > height)
        throw std::invalid_argument("Invalid TIFF request geometry");

    const TiffDirectory* selected = &base;
    Wide selected_area = Wide{width} * height;
    for (const auto& level : metadata.levels) {
        validate_directory(level);
        const auto lw = level.display_width(), lh = level.display_height();
        if (level.orientation != base.orientation || lw > width || lh > height ||
            (lw == width && lh == height))
            throw std::invalid_argument("Invalid TIFF pyramid level geometry");
        const Wide area = Wide{lw} * lh;
        // Enclosing integer read pixels are NOT the available crop resolution.
        if (area < selected_area &&
            Wide{crop.width} * lw >= Wide{geometry.output_width} * width &&
            Wide{crop.height} * lh >= Wide{geometry.output_height} * height) {
            selected = &level;
            selected_area = area;
        }
    }
    const auto x = plan_axis(crop.x, crop.width, width, selected->display_width());
    const auto y = plan_axis(crop.y, crop.height, height, selected->display_height());
    return {*selected, {x.origin, y.origin, x.length, y.length}, x.window, y.window};
}
} // namespace sipi
