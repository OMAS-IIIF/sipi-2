#include "ImageTransform.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numbers>
#include <optional>
#include <utility>

namespace sipi {
namespace {
std::optional<QuarterTurn> exact_turn(double degrees) {
    if (!std::isfinite(degrees) || degrees < 0 || degrees > 360)
        throw TransformError("Invalid rotation angle");
    if (degrees == 0 || degrees == 360) return QuarterTurn::none;
    if (degrees == 90) return QuarterTurn::clockwise90;
    if (degrees == 180) return QuarterTurn::half_turn;
    if (degrees == 270) return QuarterTurn::clockwise270;
    return {};
}
std::uint8_t quantize(double value) {
    return static_cast<std::uint8_t>(std::clamp(std::floor(value + .5), 0., 255.));
}
}
RotatedSize rotated_size(std::uint32_t width, std::uint32_t height, double degrees) {
    if (!width || !height) throw TransformError("Invalid rotation dimensions");
    if (const auto turn = exact_turn(degrees)) {
        const bool swap = *turn == QuarterTurn::clockwise90 || *turn == QuarterTurn::clockwise270;
        return {swap ? height : width, swap ? width : height};
    }
    const double radians = degrees * std::numbers::pi / 180.;
    const double c = std::abs(std::cos(radians)), s = std::abs(std::sin(radians));
    const double w = std::ceil(width*c + height*s), h = std::ceil(width*s + height*c);
    if (w > std::numeric_limits<std::uint32_t>::max() || h > std::numeric_limits<std::uint32_t>::max())
        throw TransformError("Rotated dimensions exceed limit");
    return {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h)};
}
RasterImage8 transform_image(RasterImage8 image, bool mirrored, double degrees, std::size_t limit) {
    if (const auto turn = exact_turn(degrees))
        return transform_orthogonal(std::move(image), mirrored, *turn, limit);
    const auto size = rotated_size(image.width, image.height, degrees);
    const auto channels = image.channel_count();
    const auto count = std::uint64_t(image.width) * image.height;
    if (!channels || count > limit / channels || count*channels != image.pixels.size() ||
        image.icc_profile.size() > limit - image.pixels.size())
        throw TransformError("Invalid rotation raster or memory limit exceeded");
    const bool gray = channels <= 2;
    const bool alpha = channels == 2 || channels == 4;
    const std::size_t colors = gray ? 1 : 3, out_channels = colors + 1;
    const auto available = limit - image.pixels.size() - image.icc_profile.size();
    const auto out_count = std::uint64_t(size.width) * size.height;
    if (out_count > available / out_channels)
        throw TransformError("Rotated output exceeds working memory limit");
    RasterImage8 output{size.width,size.height,
        std::vector<std::uint8_t>(static_cast<std::size_t>(out_count)*out_channels),
        std::move(image.icc_profile), gray ? PixelLayout::gray_alpha : PixelLayout::rgba, image.srgb_intent};
    const double radians = degrees * std::numbers::pi / 180.;
    const double c = std::cos(radians), s = std::sin(radians);
    for (std::uint32_t y=0; y<size.height; ++y) for (std::uint32_t x=0; x<size.width; ++x) {
        // Pixel-center inverse mapping in top-left coordinates (positive y down).
        const double dx = x + .5 - size.width/2., dy = y + .5 - size.height/2.;
        const double sx = c*dx + s*dy + image.width/2. - .5;
        const double sy = -s*dx + c*dy + image.height/2. - .5;
        const auto ix = static_cast<std::int64_t>(std::floor(sx)), iy = static_cast<std::int64_t>(std::floor(sy));
        const double fx = sx-ix, fy = sy-iy;
        double coverage = 0;
        std::array<double,3> sum{};
        for (int row=0; row<2; ++row) for (int column=0; column<2; ++column) {
            const auto px = ix+column, py = iy+row;
            if (px < 0 || py < 0 || px >= image.width || py >= image.height) continue;
            const auto source_x = mirrored ? image.width-1-px : px;
            const auto offset = (std::size_t(py)*image.width + std::size_t(source_x))*channels;
            const double weight = (column ? fx : 1-fx) * (row ? fy : 1-fy);
            const double weighted_alpha = weight * (alpha ? image.pixels[offset+colors] : 255);
            coverage += weighted_alpha;
            for (std::size_t channel=0; channel<colors; ++channel)
                sum[channel] += weighted_alpha * image.pixels[offset+channel];
        }
        const auto offset = (std::size_t(y)*size.width+x)*out_channels;
        const auto a = quantize(coverage);
        output.pixels[offset+colors] = a;
        for (std::size_t channel=0; channel<colors; ++channel)
            output.pixels[offset+channel] = a ? quantize(sum[channel]/coverage) : 0;
    }
    return output;
}
}
