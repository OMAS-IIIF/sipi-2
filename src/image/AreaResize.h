#pragma once
#include "RgbImage.h"
#include "PixelWindow.h"
#include <cstddef>
#include <stdexcept>

namespace sipi {
/** Invalid raster/dimensions or exceeded resize working-memory budget. */
class ResizeError : public std::runtime_error { public: using std::runtime_error::runtime_error; };

/** Downsample RGB8 by exact pixel-area overlap in stored channel values.
 * Both target axes must be positive and no larger than the source. Each output
 * pixel averages its entire source footprint; quantization rounds once, half up.
 * This is NOT linear-light resampling: no transfer function/profile is assumed.
 * ICC bytes move unchanged. No scaling intermediate or contribution table exists.
 *
 * Takes ownership by value; pass std::move to avoid an input copy. Identity returns
 * the original storage. The budget bounds source + output pixel bytes + ICC bytes
 * (identity needs only source + ICC), excluding vector capacity/allocator overhead.
 * Malformed rasters, enlargement and exceeded budgets throw ResizeError before
 * output allocation. Allocation failures may propagate. No shared mutable state.
 */
[[nodiscard]] RgbImage8 downsample_area(RgbImage8 image, std::uint32_t width,
    std::uint32_t height, std::size_t max_working_bytes = 256 * 1024 * 1024);
/** Downsample an exact fractional window, excluding enclosing edge pixels outside
 * it. Same ownership, stored-channel and memory semantics as the full-raster
 * overload. Each output axis must fit the exact window extent, not its rounded
 * enclosure. Invalid intervals (including zero denominators) throw ResizeError.
 * Integer quotient/remainder arithmetic determines contributing pixels exactly;
 * floating point is used only to accumulate their area weights. */
[[nodiscard]] RgbImage8 downsample_area(RgbImage8 image, const PixelWindow& window,
    std::uint32_t width, std::uint32_t height,
    std::size_t max_working_bytes = 256 * 1024 * 1024);
} // namespace sipi
