#pragma once

#include <cstdint>

namespace sipi {

/** Integer rectangle in pixels. Geometry resolution uses normalized coordinates;
 * decoders must document supported orientation. Width/height must be nonzero
 * for reading. This value owns no resources and is independent of IIIF/codecs. */
struct PixelRegion {
    std::uint32_t x, y, width, height;
};

} // namespace sipi
