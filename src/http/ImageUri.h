#pragma once

#include "iiifparser/IIIFGeometry.h"
#include <string>
#include <string_view>

namespace sipi::http {
/** Public service identity, shared by info.json and image links.
 * Inputs are a parsed (once-decoded) resource and validated public base URL
 * without a trailing slash. Each segment is encoded independently; storage paths
 * and request headers never enter this function. Returns an owning string.
 */
[[nodiscard]] std::string service_uri(const iiif::Resource& resource,
                                      std::string_view public_base_url);

/** Canonical Image API 3 URI for an authorized, successfully resolved request.
 * geometry must be the result of resolving this request against width/height and
 * policy. Dimensions describe the effective source; size is BEFORE rotation.
 * native_gray means gray conversion is an identity, as in the current renderer.
 * Uses the same size resolver to recognize max/^max under the active limits.
 * Pure, thread-safe and format-neutral; no I/O or retained references. Does not
 * authorize or check codec support. Invalid caller invariants may throw.
 */
[[nodiscard]] std::string canonical_image_uri(const iiif::ImageRequest& request,
    const iiif::ImageGeometry& geometry, std::uint32_t width, std::uint32_t height,
    const iiif::SizePolicy& policy, bool native_gray, std::string_view public_base_url);
} // namespace sipi::http
