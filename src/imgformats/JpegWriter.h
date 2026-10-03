#pragma once

#include "image/RgbImage.h"
#include <functional>
#include <span>
#include <stdexcept>

namespace sipi {

/** Invalid raster/options or a JPEG codec failure. Sink exceptions propagate unchanged. */
class JpegWriteError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Synchronous byte consumer. The span is valid only during the call; copy or
 * consume it immediately. Throw to abort. No HTTP/file/cache ownership is implied. */
using JpegSink = std::function<void(std::span<const std::uint8_t>)>;

/** Encode an owning RGB8 raster as baseline JPEG, emitting bounded byte blocks.
 * Input and sink must live for this call; independent calls share no codec state.
 * quality is 1..100 (default 90), with 4:4:4 sampling to retain chroma detail.
 * Embedded ICC bytes are preserved in APP2 markers; no color conversion occurs.
 * Untagged RGB stays untagged. EXIF/IPTC and other source metadata are not copied.
 *
 * Dimensions must fit libjpeg (1..65500) and pixels must contain exactly w*h*3
 * bytes. ICC data must fit at most 255 JPEG ICC chunks. Failures can follow partial
 * output: the caller must discard/abort that output, never publish it as complete.
 * The encoder buffers blocks, not the complete JPEG. Sink/storage limits belong
 * to the caller. No exception crosses a C callback and codec resources use RAII.
 */
void write_jpeg(const RgbImage8& image, const JpegSink& sink, int quality = 90);

} // namespace sipi
