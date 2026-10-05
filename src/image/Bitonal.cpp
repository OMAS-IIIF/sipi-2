#include "Bitonal.h"
#include "AlphaComposite.h"
#include <utility>
#include <algorithm>

namespace sipi {
RasterImage8 convert_to_bitonal(RasterImage8 image, BitonalMethod method, std::size_t max_working_bytes) {
    if (image.layout != PixelLayout::gray && image.layout != PixelLayout::gray_alpha)
        throw BitonalError("Bitonal conversion requires grayscale input");
    if (image.srgb_intent && (*image.srgb_intent > 3 || !image.icc_profile.empty()))
        throw BitonalError("Invalid or conflicting color declarations");
    if (method != BitonalMethod::threshold && method != BitonalMethod::floyd_steinberg)
        throw BitonalError("Unknown bitonal method");
    const auto row_size = static_cast<std::uint64_t>(image.width) + 2;
    const auto scratch_bytes = method == BitonalMethod::floyd_steinberg
        ? row_size * 2 * sizeof(double) : 0;
    if (scratch_bytes > max_working_bytes)
        throw BitonalError("Bitonal error rows exceed memory limit");
    // Reserve scratch in the budget before validating/compacting the input raster.
    // Removing alpha before thresholding keeps even translucent edges two-tone.
    image = composite_on_white(std::move(image), max_working_bytes - scratch_bytes);
    if (method == BitonalMethod::threshold) {
        for (auto& sample : image.pixels)
            sample = sample < 128 ? 0 : 255;
        return image;
    }

    // Padding receives off-image error, which is discarded rather than renormalized.
    std::vector<double> current(static_cast<std::size_t>(row_size), 0.0);
    std::vector<double> next(current.size(), 0.0);
    for (std::size_t y = 0; y < image.height; ++y) {
        const bool reverse = y % 2 != 0;
        for (std::size_t step = 0; step < image.width; ++step) {
            const auto x = reverse ? image.width - 1 - step : step;
            const auto i = x + 1;
            auto& sample = image.pixels[y * image.width + x];
            const double value = sample + current[i];
            sample = value < 128.0 ? 0 : 255;
            // Do not clamp intermediate values: that would discard quantization error.
            const double error = (value - sample) / 16.0;
            const auto ahead = reverse ? i - 1 : i + 1;
            const auto behind = reverse ? i + 1 : i - 1;
            current[ahead] += error * 7;
            next[behind] += error * 3;
            next[i] += error * 5;
            next[ahead] += error;
        }
        current.swap(next);
        std::fill(next.begin(), next.end(), 0.0);
    }
    return image;
}
} // namespace sipi
