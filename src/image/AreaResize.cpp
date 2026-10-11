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
        if (channels == 4 || channels == 2) {
            weight *= pixel[channels-1];
            alpha += weight;
        }
        for (std::size_t c = 0; c < (channels <= 2 ? 1 : 3); ++c) colors[c] += pixel[c] * weight;
    }
    void store(std::uint8_t* pixel, double area, std::size_t channels) const {
        if (channels == 4 || channels == 2) {
            pixel[channels-1] = quantize(alpha / area);
            // Fully transparent output has deterministic black hidden color.
            for (std::size_t c = 0; c < channels-1; ++c)
                pixel[c] = pixel[channels-1] ? quantize(colors[c] / alpha) : 0;
        } else {
            for (std::size_t c = 0; c < channels; ++c) pixel[c] = quantize(colors[c] / area);
        }
    }
};

/** One output sample's support. Only the two edges can have partial weights;
 * every interior sample has the same weight. A linear filter has at most two
 * samples. Keep the original multiplication and accumulation order per pixel. */
struct AxisWeights {
    std::uint64_t first, last;
    double first_weight, last_weight, interior_weight;

    [[nodiscard]] double weight(std::uint64_t pixel) const {
        if (pixel == first) return first_weight;
        if (pixel == last) return last_weight;
        return interior_weight;
    }
};

/** Horizontal supports are reused by every output row. Cache them only within
 * the remaining working-memory budget; tight budgets retain on-demand operation.
 * Vertical supports are computed once per row and need no table. */
template<class MakeWeights>
std::vector<AxisWeights> horizontal_weights(std::uint32_t count, std::size_t spare,
                                            MakeWeights make) {
    if (count > spare / sizeof(AxisWeights)) return {};
    std::vector<AxisWeights> weights(count);
    for (std::uint32_t x = 0; x < count; ++x) weights[x] = make(x);
    return weights;
}

/** Full-image area weights remain integers in units of target pixels, converted
 * to double exactly where the original kernel converted each overlap. */
AxisWeights integer_weights(std::uint32_t source, std::uint32_t target, std::uint32_t i) {
    const auto left = static_cast<std::uint64_t>(i) * source;
    const auto right = (static_cast<std::uint64_t>(i) + 1) * source;
    const auto first = left / target, last = (right - 1) / target;
    const auto weight = [&](std::uint64_t pixel) {
        return static_cast<double>(std::min(right, (pixel + 1) * target) -
                                   std::max(left, pixel * target));
    };
    return {first, last, weight(first), weight(last), static_cast<double>(target)};
}

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
    const auto make_horizontal = [&](std::uint32_t x) { return integer_weights(image.width, width, x); };
    const auto horizontal = horizontal_weights(width,
        max_working_bytes - input_bytes - output_bytes - image.icc_profile.size(), make_horizontal);
    RasterImage8 output{width, height, std::vector<std::uint8_t>(output_bytes), std::move(image.icc_profile), image.layout, image.srgb_intent};
    // Use integer coordinates in units of 1/target_dimension source pixels.
    // Source pixel boundaries are multiples of target_dimension; target pixel
    // boundaries are multiples of source_dimension. Thus fractional overlaps
    // have exact integer weights, with no floating-point boundary decisions.
    const double area = static_cast<double>(image.width) * image.height;
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto vertical = integer_weights(image.height, height, y);
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto support = horizontal.empty() ? make_horizontal(x) : horizontal[x];
            WeightedPixel sum;
            for (auto sy = vertical.first; sy <= vertical.last; ++sy) {
                const double wy = vertical.weight(sy);
                for (auto sx = support.first; sx <= support.last; ++sx) {
                    const double weight = support.weight(sx) * wy;
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
                       std::uint32_t output, bool allow_enlargement = false) {
    const auto bound = static_cast<std::uint64_t>(dimension) * interval.denominator;
    if (!interval.denominator || !interval.extent_numerator ||
        interval.offset_numerator > bound || interval.extent_numerator > bound - interval.offset_numerator ||
        (!allow_enlargement && interval.extent_numerator < static_cast<std::uint64_t>(output) * interval.denominator))
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
AxisWeights area_weights(const PixelInterval& interval, std::uint32_t i, std::uint32_t count) {
    const auto left = boundary(interval, i, count);
    const auto right = boundary(interval, i + 1, count);
    const auto last = right.ceiling() - 1;
    return {left.pixel, last, overlap(left.pixel, left, right), overlap(last, left, right), 1.0};
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
    const auto make_horizontal = [&](std::uint32_t x) { return area_weights(window.horizontal, x, width); };
    const auto horizontal = horizontal_weights(width,
        max_working_bytes - input_bytes - output_bytes - image.icc_profile.size(), make_horizontal);
    RasterImage8 output{width, height, std::vector<std::uint8_t>(output_bytes), std::move(image.icc_profile), image.layout, image.srgb_intent};
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto vertical = area_weights(window.vertical, y, height);
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto support = horizontal.empty() ? make_horizontal(x) : horizontal[x];
            WeightedPixel sum;
            double area = 0;
            for (auto sy = vertical.first; sy <= vertical.last; ++sy) {
                const double wy = vertical.weight(sy);
                for (auto sx = support.first; sx <= support.last; ++sx) {
                    const double weight = wy * support.weight(sx);
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
namespace {
/** Growing axes use clamped source-pixel centers; shrinking axes retain area
 * weights. Both fit the same compact support without a second pixel buffer. */
AxisWeights filter_axis(const PixelInterval& interval, std::uint32_t index,
                       std::uint32_t count) {
    if (interval.extent_numerator >= std::uint64_t{count} * interval.denominator)
        return area_weights(interval, index, count);
    const auto left = boundary(interval, index, count);

    const auto crop_first = interval.offset_numerator / interval.denominator;
    const auto crop_end = interval.offset_numerator + interval.extent_numerator;
    const auto crop_last = crop_end / interval.denominator +
                          (crop_end % interval.denominator != 0) - 1;
    const double center = static_cast<double>(left.pixel) +
        static_cast<double>(left.remainder) / static_cast<double>(left.denominator) +
        0.5 * static_cast<double>(interval.extent_numerator) / interval.denominator / count - 0.5;
    const double clamped = std::clamp(center, static_cast<double>(crop_first),
                                             static_cast<double>(crop_last));
    const auto first = static_cast<std::uint64_t>(std::floor(clamped));
    const auto last = std::min(first + 1, crop_last);
    const double fraction = clamped - first;
    return {first, last, first == last ? 1.0 : 1.0 - fraction, fraction, 0.0};
}
} // namespace

RasterImage8 resize_image(RasterImage8 image, const PixelWindow& window,
                          std::uint32_t width, std::uint32_t height,
                          std::size_t max_working_bytes) {
    const auto channels = image.channel_count();
    const auto input_bytes = pixel_bytes(image.width, image.height, channels);
    const auto output_bytes = pixel_bytes(width, height, channels);
    if (image.pixels.size() != input_bytes) throw ResizeError("Invalid raster");
    validate_interval(window.horizontal, image.width, width, true);
    validate_interval(window.vertical, image.height, height, true);
    if (window.horizontal.extent_numerator >= std::uint64_t{width} * window.horizontal.denominator &&
        window.vertical.extent_numerator >= std::uint64_t{height} * window.vertical.denominator)
        return downsample_area(std::move(image), window, width, height, max_working_bytes);
    if (input_bytes > max_working_bytes || image.icc_profile.size() > max_working_bytes - input_bytes ||
        output_bytes > max_working_bytes - input_bytes - image.icc_profile.size())
        throw ResizeError("Resize working memory exceeds limit");
    const auto make_horizontal = [&](std::uint32_t x) { return filter_axis(window.horizontal, x, width); };
    const auto weights = horizontal_weights(width,
        max_working_bytes - input_bytes - output_bytes - image.icc_profile.size(), make_horizontal);
    RasterImage8 output{width, height, std::vector<std::uint8_t>(output_bytes),
                        std::move(image.icc_profile), image.layout, image.srgb_intent};
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto vertical = filter_axis(window.vertical, y, height);
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto horizontal = weights.empty() ? make_horizontal(x) : weights[x];
            WeightedPixel sum;
            double total = 0;
            for (auto sy = vertical.first; sy <= vertical.last; ++sy) {
                const double wy = vertical.weight(sy);
                for (auto sx = horizontal.first; sx <= horizontal.last; ++sx) {
                    const double weight = wy * horizontal.weight(sx);
                    total += weight;
                    const auto offset = static_cast<std::size_t>(sy * image.width + sx) * channels;
                    sum.add(image.pixels.data() + offset, weight, channels);
                }
            }
            const auto offset = (static_cast<std::size_t>(y) * width + x) * channels;
            sum.store(output.pixels.data() + offset, total, channels);
        }
    }
    return output;
}
} // namespace sipi
