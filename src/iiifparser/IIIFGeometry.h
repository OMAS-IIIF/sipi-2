#pragma once

#include "IIIFParser.h"

#include <optional>

namespace sipi::iiif {

/** Policy applied to the scaled region, before rotation. Unset limits impose no
 * additional bound beyond uint32_t representation. max_height requires max_width;
 * absent max_height defaults to max_width, as specified by IIIF. */
struct SizePolicy {
    bool allow_upscaling = false;
    std::optional<std::uint32_t> max_width;
    std::optional<std::uint32_t> max_height;
    std::optional<std::uint64_t> max_area;
};

/** Concrete crop in the orientation-normalized full image, followed by the output
 * size before mirroring/rotation. This does not select a decoder or pyramid level. */
struct ImageGeometry {
    PixelRegion region;
    std::uint32_t output_width;
    std::uint32_t output_height;
};

enum class GeometryErrorCode {
    invalid_dimensions, invalid_region, invalid_size, upscaling_disabled,
    size_limit_exceeded, invalid_policy, unbounded_maximum
};
struct GeometryError {
    GeometryErrorCode code;
    std::string message;
};
using GeometryResult = std::variant<ImageGeometry, GeometryError>;

/** Resolve parsed region/size against nonzero, orientation-normalized dimensions.
 * Pure and thread-safe; no I/O, no mutation of input. Regions clip at image edges.
 * Percentage regions use floor for origins and ceil for extents; output sizes
 * use nearest integer (ties up); adaptive max/fit retain at least one pixel
 * per axis and reapply area limits. Explicit dimensions rounding to zero fail.
 * Invalid manually constructed values also return an error. ^max requires a
 * configured finite size limit, since an unlimited maximum cannot be computed.
 * Success confirms geometry only, not image decodability or output codec support.
 */
[[nodiscard]] GeometryResult resolve_geometry(const Region& region, const Size& size,
                                               std::uint32_t width, std::uint32_t height,
                                               const SizePolicy& policy = {});

} // namespace sipi::iiif
