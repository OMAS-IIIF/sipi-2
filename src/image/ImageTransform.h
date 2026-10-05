#pragma once
#include "OrthogonalTransform.h"

namespace sipi {
struct RotatedSize { std::uint32_t width, height; };
/** Bounding rectangle of the rotated pixel-edge rectangle, rounded up. Exact
 * multiples of 90 (including 360) retain integer dimensions, without tolerance.
 * Reject zero dimensions, nonfinite/out-of-range angles and uint32 overflow. */
[[nodiscard]] RotatedSize rotated_size(std::uint32_t width, std::uint32_t height, double degrees);

/** Mirror horizontally, then rotate clockwise about the image center.
 * Quarter turns delegate to the exact existing implementation. Other angles use
 * inverse pixel-center mapping and bilinear, alpha-weighted stored samples, with
 * transparent black outside the source. RGB becomes RGBA; gray becomes gray-alpha.
 * Return straight alpha; zero-alpha hidden color is black. No gamma/ICC transform.
 * Preserve ICC/sRGB intent. Budget includes input + output + ICC; native allocator
 * overhead/capacity is additional. Validate before output allocation; no shared
 * state or borrowed output. Invalid inputs/budget throw TransformError.
 */
[[nodiscard]] RasterImage8 transform_image(RasterImage8 image, bool mirrored, double degrees,
    std::size_t max_working_bytes = 256 * 1024 * 1024);
}
