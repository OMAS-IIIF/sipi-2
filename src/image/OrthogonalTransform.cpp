#include "OrthogonalTransform.h"
#include <algorithm>
#include <utility>

namespace sipi {
RasterImage8 transform_orthogonal(RasterImage8 image, bool mirrored,
                                  QuarterTurn rotation, std::size_t max_working_bytes) {
    if (rotation != QuarterTurn::none && rotation != QuarterTurn::clockwise90 &&
        rotation != QuarterTurn::half_turn && rotation != QuarterTurn::clockwise270)
        throw TransformError("Invalid orthogonal rotation");
    const auto channels = image.channel_count();
    const auto count = static_cast<std::uint64_t>(image.width) * image.height;
    if (!channels || !image.width || !image.height || count > max_working_bytes / channels ||
        count * channels != image.pixels.size() ||
        image.icc_profile.size() > max_working_bytes - image.pixels.size())
        throw TransformError("Invalid raster or transform memory limit exceeded");
    const bool swaps_axes = rotation == QuarterTurn::clockwise90 || rotation == QuarterTurn::clockwise270;
    if (swaps_axes && image.pixels.size() > max_working_bytes - image.pixels.size() - image.icc_profile.size())
        throw TransformError("Transform output exceeds working memory limit");
    if (!mirrored && rotation == QuarterTurn::none) return image;

    const auto output_width = swaps_axes ? image.height : image.width;
    std::vector<std::uint8_t> output;
    if (swaps_axes) output.resize(image.pixels.size());
    for (std::uint32_t y = 0; y < image.height; ++y) {
        for (std::uint32_t x = 0; x < image.width; ++x) {
            const auto mx = mirrored ? image.width - 1 - x : x;
            std::uint32_t dx = mx, dy = y;
            switch (rotation) {
            case QuarterTurn::none: break;
            case QuarterTurn::clockwise90: dx = image.height - 1 - y; dy = mx; break;
            case QuarterTurn::half_turn: dx = image.width - 1 - mx; dy = image.height - 1 - y; break;
            case QuarterTurn::clockwise270: dx = y; dy = image.width - 1 - mx; break;
            }
            // Validation bounds all products by the actual size_t-sized buffer.
            const auto from = (static_cast<std::size_t>(y)*image.width + x)*channels;
            const auto to = (static_cast<std::size_t>(dy)*output_width + dx)*channels;
            if (swaps_axes) {
                std::copy_n(image.pixels.data()+from, channels, output.data()+to);
            } else if (to > from) {
                // These mappings are involutions: swap each pair exactly once.
                for (std::size_t c = 0; c < channels; ++c)
                    std::swap(image.pixels[from+c], image.pixels[to+c]);
            }
        }
    }
    if (swaps_axes) {
        image.pixels = std::move(output);
        std::swap(image.width, image.height);
    }
    return image;
}
} // namespace sipi
