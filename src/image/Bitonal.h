#pragma once
#include "RasterImage.h"
#include <stdexcept>

namespace sipi {
/** Server rendering policy; the IIIF quality remains "bitonal" in both modes. */
enum class BitonalMethod { threshold, floyd_steinberg };

class BitonalError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Consume gray/gray-alpha8 and return opaque gray8 containing only 0 and 255.
 * Composite alpha onto white in stored sample space first, then threshold:
 * values <128 become black; values >=128 become white. Optional Floyd-Steinberg
 * diffuses quantization error with serpentine rows, starting left-to-right.
 * Diffusion uses stored samples, not linear light, and resets for each request;
 * independently rendered tiles need not match a crop of a full-image rendering.
 * RGB must pass through convert_to_gray first. ICC/sRGB declarations are retained.
 * Storage is reused; pass std::move to transfer ownership. The budget counts input
 * pixels, ICC bytes and two padded double error rows, excluding capacity/allocator
 * overhead. Invalid method, scratch budget, layout or
 * color declarations throw BitonalError; malformed dimensions/storage or exceeded
 * budget throw CompositeError (from the shared white compositor), before mutation.
 */
[[nodiscard]] RasterImage8 convert_to_bitonal(RasterImage8 image,
    BitonalMethod method = BitonalMethod::threshold,
    std::size_t max_working_bytes = 256 * 1024 * 1024);
} // namespace sipi
