#pragma once

#include "RasterImage.h"
#include <stdexcept>

namespace sipi {
/** Clockwise rotations in top-left raster coordinates; no interpolation. */
enum class QuarterTurn { none, clockwise90, half_turn, clockwise270 };

class TransformError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Consume an owning raster, mirror left-to-right if requested, then rotate.
 * Supports packed gray/gray-alpha/RGB/straight-RGBA8; copies whole pixels exactly, including
 * hidden color under transparent alpha. No filtering, color conversion, alpha
 * compositing or padding. ICC bytes and layout are unchanged. Call after crop/size.
 *
 * Identity, mirroring and 180 degrees reuse the input pixel storage. 90/270 degrees
 * allocate one equally sized output buffer, fuse optional mirroring into the same
 * pass, and swap dimensions. Pass std::move to avoid an input copy. No shared state.
 *
 * Budget counts input + ICC + output pixels when a second buffer is needed; it
 * excludes vector capacity and allocator overhead. Invalid enum/layout/dimensions,
 * wrong pixel length and budget excess throw TransformError before mutation or
 * output allocation. Allocation failures may propagate. No HTTP/parser dependency.
 */
[[nodiscard]] RasterImage8 transform_orthogonal(RasterImage8 image, bool mirrored,
    QuarterTurn rotation, std::size_t max_working_bytes = 256 * 1024 * 1024);
} // namespace sipi
