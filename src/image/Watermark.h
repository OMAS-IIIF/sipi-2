#pragma once
#include "RasterImage.h"
#include <span>
#include <memory>
#include <stdexcept>

namespace sipi {
class WatermarkError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Shared immutable original coverage mask plus its rectangle in full-image coordinates. The gray bytes
 * mean coverage, not luminance; no ICC/sRGB declaration is permitted. Treat the
 * prepared value as immutable when sharing between independent crop operations. */
struct PreparedWatermark {
    std::shared_ptr<const RasterImage8> mask;
    std::uint32_t canvas_width, canvas_height;
    std::uint32_t x, y;
    std::uint32_t width, height; // Positioned extent, independent of mask dimensions.
};

/** Center an owning gray8 mask on the full image. width_fraction is in (0,1];
 * fit height as well, preserving aspect ratio subject to integer rounding.
 * Round dimensions half up (minimum one pixel), place an odd spare pixel on the
 * right/bottom. Retain original mask storage without resizing or allocating pixels.
 * Budget covers original mask bytes; allocator capacity/overhead is additional.
 * No file I/O or color interpretation. Throws WatermarkError (allocation errors
 * may propagate). Canvas size does not increase preparation memory. */
[[nodiscard]] PreparedWatermark prepare_watermark(RasterImage8 mask,
    std::uint32_t canvas_width, std::uint32_t canvas_height, double width_fraction,
    std::size_t max_working_bytes = 256 * 1024 * 1024);

/** Share an already decoded mask without copying its pixels. Keeps it alive through
 * preparation/compositing and after cache eviction. A null pointer is invalid;
 * callers must not mutate the raster via another alias. Same validation/budget
 * and placement rules as the owning-raster overload. */
[[nodiscard]] PreparedWatermark prepare_watermark(std::shared_ptr<const RasterImage8> mask,
    std::uint32_t canvas_width, std::uint32_t canvas_height, double width_fraction,
    std::size_t max_working_bytes = 256 * 1024 * 1024);

/** Complete decoded resolution dimensions and crop origin within that resolution.
 * Each level covers the whole canvas proportionally (including odd dimensions).
 * These are not tile dimensions, IIIF output dimensions or a J2K reference-grid
 * origin. Both level axes must be no larger than the prepared full canvas. */
struct WatermarkGrid {
    std::uint32_t width, height;
    std::uint32_t origin_x = 0, origin_y = 0;
};

/** Apply to an enclosing decoded crop on a reduced resolution grid. Map each
 * level pixel directly into the ORIGINAL mask, using area filtering on shrinking
 * axes and linear interpolation on enlarged axes. Outside the positioned mask
 * contributes zero; boundary pixels use the visible footprint and its area share.
 * Canvas edges are clipped with integer arithmetic before source mapping. No
 * resized mask or contribution table is allocated. Coverage stays double until
 * compositing; results can differ from the former two-stage quantized filtering.
 * Apply BEFORE the read plan's fractional window/final resize and transforms.
 * Identical level pixels agree across independently decoded crops. This does not
 * assert that compositing and image downsampling commute: source/alpha variation
 * and existing pyramid filters can cause differences between resolution levels.
 * Other color, ownership, memory and error rules match the native overload. */
[[nodiscard]] RasterImage8 apply_watermark(RasterImage8 image,
    const PreparedWatermark& watermark, std::span<const std::uint8_t> color,
    double opacity, const WatermarkGrid& grid,
    std::size_t max_working_bytes = 256 * 1024 * 1024);

/** Composite a prepared mask over an owning full raster or an unscaled crop at
 * (origin_x,origin_y) in the SAME canvas pixel grid. Color has one sample for gray
 * or three for RGB, already in the destination's stored color encoding. This is
 * stored-sample source-over, not linear-light compositing or ICC conversion.
 * opacity is finite in [0,1]; mask coverage multiplies opacity. Straight alpha
 * is respected, including hidden destination colors. Zero coverage leaves bytes
 * untouched. ICC/sRGB declarations and layout are preserved.
 * Consumes image storage in place, borrows mask/color only during the call; no
 * shared mutation. Budget includes image pixels/profile plus prepared mask.
 * Invalid inputs throw WatermarkError before mutation. Crop then resize/rotate
 * the composited result; use the WatermarkGrid overload for reduced pyramid grids. */
[[nodiscard]] RasterImage8 apply_watermark(RasterImage8 image,
    const PreparedWatermark& watermark, std::span<const std::uint8_t> color,
    double opacity, std::uint32_t origin_x = 0, std::uint32_t origin_y = 0,
    std::size_t max_working_bytes = 256 * 1024 * 1024);
}
