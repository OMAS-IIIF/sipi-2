#pragma once

#include "image/RasterImage.h"
#include "iiifparser/IIIFGeometry.h"
#include "storage/ByteSource.h"
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace sipi::cache {

/** Bump when fixed rendering behavior or codec changes can alter output bytes.
 * v1: current TIFF/Grok level planning; stored-sample area filtering; preserved
 * ICC; straight alpha; JPEG white composition, baseline 4:4:4; PNG level 6,
 * noninterlaced. This is independent of the cache serialization schema. */
inline constexpr std::string_view current_render_recipe = "sipi-render-v1";

/** Actual JPEG encoder quality, not the IIIF quality parameter; range 1..100.
 * No default here: callers must supply the value used for encoding. */
struct JpegEncoding { int quality; };
/** Fixed PNG parameters belong to the rendering recipe until configurable. */
struct PngEncoding {};
using Encoding = std::variant<JpegEncoding, PngEncoding>;

/** Owning identity of one authorized rendering result, BEFORE cache lookup.
 * source comes from the opened, version-bound ByteSource. geometry is already
 * resolved and policy-validated in the effective image's coordinates, before
 * mirroring/rotation. input_layout is the decoded layout before quality changes.
 *
 * variant is REQUIRED: use "original" for unrestricted source content. Future
 * restricted/virtual-image variants need a stable identity covering all their
 * output-affecting parameters (including watermark placement, if applicable).
 * Equal authorized variants may share results; never use tokens/cookies/user IDs.
 * A watermark additionally needs its definitive source identity and version.
 * recipe covers fixed encoder/color/algorithm choices; configurable choices
 * must acquire explicit fields when implemented. No policy execution occurs here.
 *
 * No Resource/URL/method fields: different public aliases may share image bytes.
 * HTTP response headers and authorization decisions must NOT be cached with them.
 */
struct RenderIdentity {
    SourceInfo source;
    std::string variant;
    iiif::ImageGeometry geometry;
    iiif::Rotation rotation;
    iiif::Quality quality;
    PixelLayout input_layout;
    Encoding encoding;
    std::optional<SourceInfo> watermark;
    std::string recipe = std::string(current_render_recipe);
};

/** Immutable identity bytes and their SHA-256 digest, owning all storage.
 * Normalizes 360/-0 to 0 degrees and resolves default/color quality against the
 * input layout (native gray stays gray). Does not merge operations whose output
 * equivalence would require inspecting pixels, e.g. symmetrical rotations.
 *
 * Serialization v1: fixed named fields, decimal byte lengths and exact payloads;
 * integers are locale-independent decimal, angles are normalized binary64 bits
 * in 16 lowercase hex digits. Embedded delimiters/NUL cannot merge field bounds.
 * The digest is 64 lowercase hex digits, suitable for future sharded filenames.
 * Keep/compare serialization in the future index as a hash-collision safeguard.
 * It may contain private source identifiers: do not expose it in HTTP or logs.
 *
 * Construction is deterministic and independently thread-safe, without I/O or
 * global mutable state. Invalid/incomplete input throws std::invalid_argument;
 * digest failure throws std::runtime_error; allocation failures may propagate.
 * A key does NOT prove authorization, geometry bounds, or renderer support. Both
 * fresh rendering and cache hits must pass the same policy/capability checks.
 */
class CacheKey {
public:
    explicit CacheKey(const RenderIdentity& identity);
    [[nodiscard]] std::string_view serialized() const noexcept { return serialized_; }
    [[nodiscard]] std::string_view digest() const noexcept { return digest_; }
    bool operator==(const CacheKey&) const = default;

private:
    std::string serialized_;
    std::string digest_;
};
} // namespace sipi::cache
