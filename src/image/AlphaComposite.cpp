#include "AlphaComposite.h"

namespace sipi {
RasterImage8 composite_on_white(RasterImage8 image, std::size_t max_working_bytes) {
    const auto channels = image.channel_count();
    const auto count = static_cast<std::uint64_t>(image.width) * image.height;
    if (!channels || !image.width || !image.height || count > max_working_bytes / channels ||
        count * channels != image.pixels.size() ||
        image.icc_profile.size() > max_working_bytes - image.pixels.size())
        throw CompositeError("Invalid raster or compositing memory limit exceeded");
    if (image.layout != PixelLayout::rgba) return image;
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned alpha = image.pixels[4*i+3];
        for (unsigned c = 0; c < 3; ++c) {
            const unsigned value = image.pixels[4*i+c];
            image.pixels[3*i+c] = static_cast<std::uint8_t>((value*alpha + 255*(255-alpha) + 127)/255);
        }
    }
    image.pixels.resize(static_cast<std::size_t>(count) * 3);
    image.layout = PixelLayout::rgb;
    return image;
}
} // namespace sipi
