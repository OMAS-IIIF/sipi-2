#pragma once

namespace sipi {

/** RGB is encoded as YCbCr: retain all chroma samples, or halve both chroma axes.
 * Grayscale has only one component and ignores this selection. */
enum class JpegSampling { yuv444, yuv420 };

/** Shared value settings for startup, encoding and persistent result identity.
 * Quality is libjpeg's 1..100 scale, not IIIF's color/gray/bitonal selector.
 * Defaults preserve the original baseline JPEG output. No mutable codec state. */
struct JpegOptions {
    int quality = 90;
    JpegSampling sampling = JpegSampling::yuv444;

    /** Validate before encoding, creating workers or constructing cache keys. */
    [[nodiscard]] bool valid() const noexcept {
        return quality >= 1 && quality <= 100 &&
               (sampling == JpegSampling::yuv444 || sampling == JpegSampling::yuv420);
    }
};

} // namespace sipi
