#ifndef SIPI_IIIF_PARSER_H
#define SIPI_IIIF_PARSER_H

#include "image/PixelRegion.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sipi::iiif {

/** Decoded resource name. Prefix segments retain their boundaries even if they
 * contain an encoded slash. These names are not filesystem paths. */
struct Resource {
    std::vector<std::string> prefix;
    std::string identifier;
};

struct FullRegion {};
struct SquareRegion {};
using PixelRegion = sipi::PixelRegion;
struct PercentRegion {
    double x, y, width, height;
};
/** Requested region; clipping and square placement require image dimensions. */
using Region = std::variant<FullRegion, SquareRegion, PixelRegion, PercentRegion>;

struct MaxSize {};
struct WidthSize { std::uint32_t width; };
struct HeightSize { std::uint32_t height; };
struct ExactSize { std::uint32_t width, height; };
struct FitSize { std::uint32_t width, height; };
struct PercentSize { double percent; };
/** Requested size, before aspect-ratio calculations, rounding, or server limits.
 * The leading '^' permits upscaling; it does not require it. */
struct Size {
    bool allow_upscaling;
    std::variant<MaxSize, WidthSize, HeightSize, ExactSize, FitSize, PercentSize> value;
};

/** Horizontal mirroring happens before clockwise rotation, in [0, 360] degrees. */
struct Rotation {
    bool mirrored;
    double degrees;
};

enum class Quality { default_quality, color, gray, bitonal };
/** Formats listed by IIIF Image API 3. Recognition does not promise a codec. */
enum class Format { jpg, tif, png, gif, jp2, pdf, webp };

struct ImageRequest {
    Resource resource;
    Region region;
    Size size;
    Rotation rotation;
    Quality quality;
    Format format;
};
struct InfoRequest { Resource resource; };

/** Identifies the component that failed, without depending on HTTP status codes. */
enum class Component { path, region, size, rotation, quality_format };
struct ParseError {
    Component component;
    std::string message;
};
using ParseResult = std::variant<ImageRequest, InfoRequest, ParseError>;

/** Parse an encoded absolute URL path for an image or info.json request.
 *
 * Input excludes scheme, host, query, and fragment. Split on literal slashes
 * before decoding each segment exactly once; '+' remains '+'. Empty segments,
 * malformed escapes, and ASCII controls (including NUL/DEL) are rejected.
 * All returned strings own their data; no input views survive the call. This
 * function has no I/O or shared state and can be called concurrently.
 *
 * Pixel values must fit uint32_t. Decimal values must be finite and representable
 * as double. Geometry, image limits, filesystem resolution, canonicalization,
 * codec availability, and base-URI redirects are deliberately separate concerns.
 * Invalid input returns ParseError; ordinary allocation failures may still throw.
 */
[[nodiscard]] ParseResult parse_path(std::string_view encoded_path);

} // namespace sipi::iiif

#endif
