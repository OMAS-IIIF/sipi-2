#include "AreaResize.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <utility>

namespace sipi {
namespace {
std::size_t pixel_bytes(std::uint32_t width, std::uint32_t height, std::size_t channels) {
    if (!channels || !width || !height || static_cast<std::uint64_t>(width) * height >
        std::numeric_limits<std::size_t>::max() / channels)
        throw ResizeError("Invalid raster dimensions/layout");
    return static_cast<std::size_t>(width) * height * channels;
}

std::uint8_t quantize(double value) {
    return static_cast<std::uint8_t>(std::clamp(std::floor(value + 0.5), 0.0, 255.0));
}

/** Accumulate premultiplied contributions in double without quantizing an
 * intermediate RGBA raster. Divide by total alpha to return straight RGB. */
struct WeightedPixel {
    std::array<double, 3> colors{};
    double alpha = 0;

    void add(const std::uint8_t* pixel, double weight, std::size_t channels) {
        if (channels == 4) {
            weight *= pixel[3];
            alpha += weight;
        }
        for (std::size_t c = 0; c < 3; ++c) colors[c] += pixel[c] * weight;
    }
    void store(std::uint8_t* pixel, double area, std::size_t channels) const {
        if (channels == 4) {
            pixel[3] = quantize(alpha / area);
            // Fully transparent output has deterministic black hidden color.
            for (std::size_t c = 0; c < 3; ++c)
                pixel[c] = pixel[3] ? quantize(colors[c] / alpha) : 0;
        } else {
            for (std::size_t c = 0; c < 3; ++c) pixel[c] = quantize(colors[c] / area);
        }
    }
};

}

RasterImage8 downsample_area(RasterImage8 image, std::uint32_t width, std::uint32_t height,
                          std::size_t max_working_bytes) {
    const auto channels = image.channel_count();
    const auto input_bytes = pixel_bytes(image.width, image.height, channels);
    const auto output_bytes = pixel_bytes(width, height, channels);
    if (image.pixels.size() != input_bytes || width > image.width || height > image.height)
        throw ResizeError("Invalid raster or enlargement requested");
    const bool identity = width == image.width && height == image.height;
    if (input_bytes > max_working_bytes || image.icc_profile.size() > max_working_bytes - input_bytes ||
        (!identity && output_bytes > max_working_bytes - input_bytes - image.icc_profile.size()))
        throw ResizeError("Resize working memory exceeds limit");
    if (identity) return image;
    RasterImage8 output{width, height, std::vector<std::uint8_t>(output_bytes), std::move(image.icc_profile), image.layout};
    // Use integer coordinates in units of 1/target_dimension source pixels.
    // Source pixel boundaries are multiples of target_dimension; target pixel
    // boundaries are multiples of source_dimension. Thus fractional overlaps
    // have exact integer weights, with no floating-point boundary decisions.
    const double area = static_cast<double>(image.width) * image.height;
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto top = static_cast<std::uint64_t>(y) * image.height;
        const auto bottom = (static_cast<std::uint64_t>(y) + 1) * image.height;
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto left = static_cast<std::uint64_t>(x) * image.width;
            const auto right = (static_cast<std::uint64_t>(x) + 1) * image.width;
            WeightedPixel sum;
            for (auto sy = top / height; sy <= (bottom - 1) / height; ++sy) {
                const auto wy = std::min(bottom, (sy + 1) * height) - std::max(top, sy * height);
                for (auto sx = left / width; sx <= (right - 1) / width; ++sx) {
                    const auto wx = std::min(right, (sx + 1) * width) - std::max(left, sx * width);
                    const double weight = static_cast<double>(wx) * static_cast<double>(wy);
                    const auto index = static_cast<std::size_t>(sy * image.width + sx) * channels;
                    sum.add(image.pixels.data() + index, weight, channels);
                }
            }
            const auto index = (static_cast<std::size_t>(y) * width + x) * channels;
            sum.store(output.pixels.data() + index, area, channels);
        }
    }
    return output;
}
namespace {
/** A boundary split into an integer pixel and a proper fraction. Splitting
 * extent before multiplying avoids a 96-bit product for large input fractions. */
struct Boundary {
    std::uint64_t pixel, remainder, denominator;
    [[nodiscard]] std::uint64_t ceiling() const { return pixel + (remainder != 0); }
};
Boundary boundary(const PixelInterval& interval, std::uint32_t i, std::uint32_t count) {
    const auto whole = interval.offset_numerator + (interval.extent_numerator / count) * i;
    const auto rest = (interval.extent_numerator % count) * i;
    const auto numerator = whole + rest / count;
    return {numerator / interval.denominator,
            (numerator % interval.denominator) * count + rest % count,
            static_cast<std::uint64_t>(interval.denominator) * count};
}

void validate_interval(const PixelInterval& interval, std::uint32_t dimension,
                       std::uint32_t output) {
    const auto bound = static_cast<std::uint64_t>(dimension) * interval.denominator;
    if (!interval.denominator || !interval.extent_numerator ||
        interval.offset_numerator > bound || interval.extent_numerator > bound - interval.offset_numerator ||
        interval.extent_numerator < static_cast<std::uint64_t>(output) * interval.denominator)
        throw ResizeError("Invalid fractional window or enlargement requested");
}

/** Work relative to the current pixel, so large absolute coordinates never
 * lose their fractional bits when converted to floating point. */
double overlap(std::uint64_t pixel, const Boundary& left, const Boundary& right) {
    const double start = pixel == left.pixel
        ? static_cast<double>(left.remainder) / static_cast<double>(left.denominator) : 0.0;
    const double end = pixel == right.pixel
        ? static_cast<double>(right.remainder) / static_cast<double>(right.denominator) : 1.0;
    return end - start;
}
} // namespace

RasterImage8 downsample_area(RasterImage8 image, const PixelWindow& window,
                          std::uint32_t width, std::uint32_t height,
                          std::size_t max_working_bytes) {
    const auto channels = image.channel_count();
    const auto input_bytes = pixel_bytes(image.width, image.height, channels);
    const auto output_bytes = pixel_bytes(width, height, channels);
    if (image.pixels.size() != input_bytes) throw ResizeError("Invalid raster");
    validate_interval(window.horizontal, image.width, width);
    validate_interval(window.vertical, image.height, height);
    const auto full_axis = [](const PixelInterval& axis, std::uint32_t dimension) {
        return axis.offset_numerator == 0 && axis.extent_numerator ==
            static_cast<std::uint64_t>(dimension) * axis.denominator;
    };
    if (full_axis(window.horizontal, image.width) && full_axis(window.vertical, image.height))
        return downsample_area(std::move(image), width, height, max_working_bytes);
    if (input_bytes > max_working_bytes || image.icc_profile.size() > max_working_bytes - input_bytes ||
        output_bytes > max_working_bytes - input_bytes - image.icc_profile.size())
        throw ResizeError("Resize working memory exceeds limit");
    RasterImage8 output{width, height, std::vector<std::uint8_t>(output_bytes), std::move(image.icc_profile), image.layout};
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto top = boundary(window.vertical, y, height);
        const auto bottom = boundary(window.vertical, y + 1, height);
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto left = boundary(window.horizontal, x, width);
            const auto right = boundary(window.horizontal, x + 1, width);
            WeightedPixel sum;
            double area = 0;
            for (auto sy = top.pixel; sy < bottom.ceiling(); ++sy) {
                const auto wy = overlap(sy, top, bottom);
                for (auto sx = left.pixel; sx < right.ceiling(); ++sx) {
                    const double weight = wy * overlap(sx, left, right);
                    const auto index = static_cast<std::size_t>(sy * image.width + sx) * channels;
                    area += weight;
                    sum.add(image.pixels.data() + index, weight, channels);
                }
            }
            const auto index = (static_cast<std::size_t>(y) * width + x) * channels;
            sum.store(output.pixels.data() + index, area, channels);
        }
    }
    return output;
}
} // namespace sipi
