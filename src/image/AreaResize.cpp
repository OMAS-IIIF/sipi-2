#include "AreaResize.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
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

/** Round half up and clamp to 0..255: floor(value + 0.5), clamped.
 * For rounded >= 0, truncation equals floor; values below 0 clamp to 0 and
 * values from 255 up clamp to 255. This avoids a libm floor() call on x86-64
 * baselines without SSE4.1 and yields exactly the same bytes. */
std::uint8_t quantize(double value) {
    const double rounded = value + 0.5;
    if (!(rounded >= 0.0)) return 0;
    if (rounded >= 255.0) return 255;
    return static_cast<std::uint8_t>(static_cast<int>(rounded));
}

/** Accumulate premultiplied contributions in double without quantizing an
 * intermediate RGBA raster. Divide by total alpha to return straight RGB.
 * Channels is a template parameter so the per-sample loop has no runtime
 * branches; the arithmetic (expressions and their order) is unchanged, so
 * results are bit-identical to a generic implementation on every platform. */
template<std::size_t Channels>
struct WeightedPixel {
    static constexpr bool has_alpha = Channels == 2 || Channels == 4;
    static constexpr std::size_t color_count = has_alpha ? Channels - 1 : Channels;
    std::array<double, color_count> colors{};
    double alpha = 0;

    void add(const std::uint8_t* pixel, double weight) {
        if constexpr (has_alpha) {
            weight *= pixel[Channels-1];
            alpha += weight;
        }
        for (std::size_t c = 0; c < color_count; ++c) colors[c] += pixel[c] * weight;
    }
    void store(std::uint8_t* pixel, double area) const {
        if constexpr (has_alpha) {
            pixel[Channels-1] = quantize(alpha / area);
            // Fully transparent output has deterministic black hidden color.
            for (std::size_t c = 0; c < color_count; ++c)
                pixel[c] = pixel[Channels-1] ? quantize(colors[c] / alpha) : 0;
        } else {
            for (std::size_t c = 0; c < Channels; ++c) pixel[c] = quantize(colors[c] / area);
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

/** Every support is expanded into one flat weight array, so the inner loop reads
 * consecutive weights instead of comparing each sample with the support edges.
 * The stored values are exactly AxisWeights::weight(sx). */
struct ExpandedSupports {
    std::vector<std::uint64_t> first;   // First source sample per output index.
    std::vector<std::size_t> offset;    // Weights of index i: [offset[i], offset[i+1]).
    std::vector<double> weights;
};

/** Expand count supports if they fit in spare bytes; otherwise return nothing
 * and let the kernel compute supports on demand (same values, more work). */
template<class MakeWeights>
std::optional<ExpandedSupports> expand_supports(std::uint32_t count, std::size_t spare, MakeWeights make) {
    std::size_t total = 0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto support = make(i);
        total += static_cast<std::size_t>(support.last - support.first + 1);
    }
    const auto per_index = sizeof(std::uint64_t) + sizeof(std::size_t);
    if (count >= spare / per_index || total > (spare - (std::size_t{count} + 1) * per_index) / sizeof(double))
        return std::nullopt;
    ExpandedSupports result;
    result.first.reserve(count);
    result.offset.reserve(std::size_t{count} + 1);
    result.weights.reserve(total);
    for (std::uint32_t i = 0; i < count; ++i) {
        const auto support = make(i);
        result.first.push_back(support.first);
        result.offset.push_back(result.weights.size());
        for (auto sample = support.first; sample <= support.last; ++sample)
            result.weights.push_back(support.weight(sample));
    }
    result.offset.push_back(result.weights.size());
    return result;
}

/** One output column's horizontal support, read from the expanded table. */
struct TableColumn {
    std::uint64_t first;
    std::size_t count;
    const double* weights;
    [[nodiscard]] double weight(std::size_t k) const { return weights[k]; }
};
/** The same support computed on demand when no table fits the budget. */
struct OnDemandColumn {
    AxisWeights support;
    std::uint64_t first;
    std::size_t count;
    explicit OnDemandColumn(const AxisWeights& weights)
        : support(weights), first(weights.first), count(weights.last - weights.first + 1) {}
    [[nodiscard]] double weight(std::size_t k) const { return support.weight(first + k); }
};

/** How the divisor of a pixel is obtained. The full-image area path divides by
 * the constant source area; window paths divide by the summed weights. */
enum class Divisor { source_area, summed_weights };

/** Shared resampling loop: every output pixel sums weight(sx) * weight(sy)
 * times each covered source sample, in source order, and divides once.
 * Per sample it performs exactly the floating-point operations of the original
 * generic kernel in the same order, so results are bit-identical; only branches
 * on the channel count and the support edges have been removed. */
template<std::size_t Channels, Divisor divisor, class ColumnAt, class MakeVertical>
void resample(const RasterImage8& image, RasterImage8& output, MakeVertical vertical,
              ColumnAt column_at, double source_area) {
    const std::size_t stride = static_cast<std::size_t>(image.width) * Channels;
    for (std::uint32_t y = 0; y < output.height; ++y) {
        const AxisWeights rows = vertical(y);
        std::uint8_t* target = output.pixels.data() + static_cast<std::size_t>(y) * output.width * Channels;
        for (std::uint32_t x = 0; x < output.width; ++x, target += Channels) {
            const auto column = column_at(x);
            WeightedPixel<Channels> sum;
            double area = 0;
            for (auto sy = rows.first; sy <= rows.last; ++sy) {
                const double wy = rows.weight(sy);
                const std::uint8_t* sample = image.pixels.data() + static_cast<std::size_t>(sy) * stride +
                                             static_cast<std::size_t>(column.first) * Channels;
                for (std::size_t k = 0; k < column.count; ++k, sample += Channels) {
                    const double weight = wy * column.weight(k);
                    if constexpr (divisor == Divisor::summed_weights) area += weight;
                    sum.add(sample, weight);
                }
            }
            sum.store(target, divisor == Divisor::source_area ? source_area : area);
        }
    }
}

/** Call run.template operator()<Channels>() for the raster's channel count. */
template<class Run>
void with_channels(std::size_t channels, Run&& run) {
    switch (channels) {
    case 1: run.template operator()<1>(); break;
    case 2: run.template operator()<2>(); break;
    case 3: run.template operator()<3>(); break;
    case 4: run.template operator()<4>(); break;
    default: throw ResizeError("Unsupported channel count");
    }
}

/** Select the channel specialization and table or on-demand supports. */
template<Divisor divisor, class MakeHorizontal, class MakeVertical>
void resample_any(const RasterImage8& image, RasterImage8& output, MakeHorizontal make_horizontal,
                  MakeVertical make_vertical, std::size_t spare, double source_area = 0) {
    const auto table = expand_supports(output.width, spare, make_horizontal);
    with_channels(image.channel_count(), [&]<std::size_t Channels>() {
        if (table) {
            const auto column = [&](std::uint32_t x) {
                return TableColumn{table->first[x], table->offset[x + 1] - table->offset[x],
                                   table->weights.data() + table->offset[x]};
            };
            resample<Channels, divisor>(image, output, make_vertical, column, source_area);
        } else {
            const auto column = [&](std::uint32_t x) { return OnDemandColumn(make_horizontal(x)); };
            resample<Channels, divisor>(image, output, make_vertical, column, source_area);
        }
    });
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
    const auto spare = max_working_bytes - input_bytes - output_bytes - image.icc_profile.size();
    RasterImage8 output{width, height, std::vector<std::uint8_t>(output_bytes), std::move(image.icc_profile), image.layout, image.srgb_intent};
    // Use integer coordinates in units of 1/target_dimension source pixels.
    // Source pixel boundaries are multiples of target_dimension; target pixel
    // boundaries are multiples of source_dimension. Thus fractional overlaps
    // have exact integer weights, with no floating-point boundary decisions.
    resample_any<Divisor::source_area>(image, output,
        [&](std::uint32_t x) { return integer_weights(image.width, width, x); },
        [&](std::uint32_t y) { return integer_weights(image.height, height, y); },
        spare, static_cast<double>(image.width) * image.height);
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
    const auto spare = max_working_bytes - input_bytes - output_bytes - image.icc_profile.size();
    RasterImage8 output{width, height, std::vector<std::uint8_t>(output_bytes), std::move(image.icc_profile), image.layout, image.srgb_intent};
    resample_any<Divisor::summed_weights>(image, output,
        [&](std::uint32_t x) { return area_weights(window.horizontal, x, width); },
        [&](std::uint32_t y) { return area_weights(window.vertical, y, height); }, spare);
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
    const auto spare = max_working_bytes - input_bytes - output_bytes - image.icc_profile.size();
    RasterImage8 output{width, height, std::vector<std::uint8_t>(output_bytes),
                        std::move(image.icc_profile), image.layout, image.srgb_intent};
    resample_any<Divisor::summed_weights>(image, output,
        [&](std::uint32_t x) { return filter_axis(window.horizontal, x, width); },
        [&](std::uint32_t y) { return filter_axis(window.vertical, y, height); }, spare);
    return output;
}
} // namespace sipi
