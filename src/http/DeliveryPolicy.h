#pragma once
#include "cache/MetadataCache.h"
#include "imgformats/TiffReadPlan.h"
#include "iiifparser/IIIFGeometry.h"
#include <stdexcept>

namespace sipi::http {
/** A deterministic request exceeds the current renderer's known budget.
 * Map to 501 for source admission (full/max unavailable), 400 for a request.
 * Corrupt compressed streams and unforeseen native failures remain reader errors.
 */
class DeliveryLimitError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Supply public defaults and validate hard ceilings before starting workers. */
[[nodiscard]] iiif::SizePolicy bounded_output_policy(iiif::SizePolicy policy);

/** Select only pixel-supported TIFF levels. Shared by budget planning and decode;
 * directories are copied, ICC bytes are not. No I/O or pixel allocation. */
[[nodiscard]] TiffReadPlan supported_tiff_plan(const TiffMetadata& metadata,
                                              const iiif::ImageGeometry& geometry);

/** Check known decode/resample/transform working sets before cache lookup or I/O.
 * Uses the same TIFF/J2K plan as decode, whole JPEG/PNG input, JPEG coefficient
 * metadata, and a conservative four-channel transformed raster. Checks worst-case
 * gray/bitonal scratch as well, keeping the public contract quality-independent.
 * Native codec/LCMS scratch, corrupt input and compressed read counts cannot be
 * guaranteed by metadata. Does not mutate or retain source metadata.
 */
void validate_delivery_budget(const cache::ImageMetadata& metadata,
                              const iiif::ImageGeometry& geometry,
                              const iiif::Rotation& rotation);
} // namespace sipi::http
