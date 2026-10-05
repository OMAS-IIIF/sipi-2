#include "AlphaComposite.h"

namespace sipi {
RasterImage8 composite_on_white(RasterImage8 image, std::size_t max_working_bytes) {
    const auto channels = image.channel_count();
    const auto count = static_cast<std::uint64_t>(image.width) * image.height;
    if (!channels || !image.width || !image.height || count > max_working_bytes / channels ||
        count * channels != image.pixels.size() ||
        image.icc_profile.size() > max_working_bytes - image.pixels.size())
        throw CompositeError("Invalid raster or compositing memory limit exceeded");
    if (channels != 4 && channels != 2) return image;
    const auto colors = channels - 1;
    for (std::size_t i = 0; i < count; ++i) {
        const unsigned alpha = image.pixels[channels*i+colors];
        for (unsigned c = 0; c < colors; ++c) {
            const unsigned value = image.pixels[channels*i+c];
            image.pixels[colors*i+c] = static_cast<std::uint8_t>((value*alpha + 255*(255-alpha) + 127)/255);
        }
    }
    image.pixels.resize(static_cast<std::size_t>(count) * colors);
    image.layout = colors == 1 ? PixelLayout::gray : PixelLayout::rgb;
    return image;
}
} // namespace sipi
