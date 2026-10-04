#pragma once
#include "RasterImage.h"
#include <stdexcept>

namespace sipi {
class CompositeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Consume straight RGBA8 and composite onto encoded RGB white (255,255,255).
 * RGB and gray input are returned unchanged. ICC is retained; processing uses the stored
 * channel encoding, NOT linear light or implicit sRGB. Alpha is coverage.
 * Compacts RGBA storage in place to RGB; no second raster is allocated. Pass
 * std::move to transfer ownership. Budget covers input pixel bytes + ICC, not
 * retained vector capacity, allocator overhead or a process-wide memory total.
 * Invalid dimensions/layout/byte count or budget throws CompositeError before
 * mutation. Returns an owning RGB or gray raster suitable for the JPEG encoder. */
[[nodiscard]] RasterImage8 composite_on_white(RasterImage8 image,
    std::size_t max_working_bytes = 256 * 1024 * 1024);
} // namespace sipi
