#pragma once
#include "image/RasterImage.h"
#include <functional>
#include <span>
#include <stdexcept>

namespace sipi {
/** Invalid raster/profile/options, color conversion failure or WebP codec/mux error. */
class WebpWriteError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
using WebpSink = std::function<void(std::span<const std::uint8_t>)>;
/** Encode a still WebP; borrows raster/sink, owns per-call codec/color/mux state.
 * RGB ICC is preserved. Gray ICC is transformed to RGB sRGB (relative intent,
 * BPC), explicit gray sRGB uses its declared curve, untagged gray assumes Gamma
 * 2.2 D50. RGB without ICC remains untagged; explicit sRGB gets an RGB ICC.
 * Gray expansion preserves alpha. No grayscale ICC is placed on RGB pixels.
 * lossless preserves prepared samples including hidden transparent color; lossy
 * uses quality 1..100, method 4 and lossless alpha. No EXIF/IPTC or animation.
 * Dimensions <=16383; input + codec ARGB + ICC <=256 MiB, ICC <=16 MiB;
 * encoded and final mux buffers each <=64 MiB. Native codec scratch is additional.
 * Complete muxed output is emitted in <=16KiB borrowed blocks. Sink exceptions
 * propagate. Discard partial output on failure; no global state or input mutation.
 */
void write_webp(const RasterImage8& image, const WebpSink& sink,
                bool lossless, int quality = 85);
inline constexpr std::uint32_t webp_maximum_dimension = 16383;
}
