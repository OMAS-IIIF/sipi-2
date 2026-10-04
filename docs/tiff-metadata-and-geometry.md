# TIFF metadata and IIIF geometry

The metadata and geometry APIs read TIFF delivery metadata and resolve parsed
region/size values without decoding pixels. A separate region reader now decodes
the supported RGB subset described below. The HTTP worker resolves the authorized
source via SourceResolver, returns info.json metadata, and validates image geometry.
Supported tiled RGB8 TIFF crops now return JPEG; other pixel operations return 501.
See [JPEG delivery](jpeg-delivery.md).

## TIFF reader

`read_tiff_metadata(source)` in `src/imgformats/TiffMetadata.h` uses libtiff 4.7.2
from Conan. It borrows a `ByteSource`, returns owning values, and releases its TIFF
handle on success and failure. The caller owns the source. `TiffReadError` reports
codec/header failures; `ByteSourceError` reports storage failures. See
[byte sources](byte-source.md). Error callbacks use bounded
per-handle storage and do not throw across C callbacks or modify global handlers.
Independent concurrent reads are supported.

The first main directory supplies:

- Stored width/height and orientation; `display_width()` / `display_height()`
  give orientation-normalized dimensions for IIIF (orientations 5..8 swap axes).
- Samples per pixel, bits per sample, sample format, photometric interpretation,
  planar configuration, compression, and extra-sample/alpha tags.
- Tile dimensions or rows per strip. Strip chopping is disabled to retain layout.
- An owning copy of the embedded ICC profile, if present. This step does not
  validate the profile or perform a color transformation.

Numeric sample and compression fields retain TIFF tag codes. Reading metadata
successfully does not prove that the encoded pixels are intact or that a future
SIPI decoder will support their representation. EXIF/IPTC/XMP preservation,
archival metadata, and original-upload storage remain deferred.

The reader discovers reduced images in consecutive main IFDs and SubIFD arrays,
including nested reduced SubIFDs. A new full main page stops main-chain discovery.
Only explicitly marked reduced images with compatible sample interpretation,
orientation, extra samples, and approximately matching aspect ratio are retained.
A level with its own differing ICC profile is excluded; absence means inheritance
from the main image for this planning step. The full image is separate from the
largest-first `levels` vector; each entry retains its directory offset.

This is conservative candidate discovery, not proof of image content equivalence.
Unmarked legacy pyramids and SubIFD linked chains are not inferred/traversed in
this first implementation. Main-IFD chains and SubIFD *arrays/trees* are supported.
A smaller ordinary page is never classified as a level solely by its dimensions.
Pyramid-level selection and conversion of display coordinates to stored/level
coordinates are future pixel-decoder work.

The reader never calls scanline, strip, or tile decoding functions. Memory mapping
is disabled, and traversal is bounded to 256 directories. libtiff allocations are
limited to 64 MiB, with a separate 16 MiB limit for copied ICC data. This bounds
metadata handling; it is not a future decoded-image memory policy. Fatal libtiff
diagnostics fail the read. Nonfatal warnings are suppressed; header recovery by
libtiff is not a promise of strict TIFF conformance.

## Pure geometry resolution

`resolve_geometry(region, size, width, height, policy)` in
`src/iiifparser/IIIFGeometry.h` returns `ImageGeometry` or `GeometryError`.
It is independent of libtiff, HTTP, filesystem access, and color management.
Inputs are interpreted against the orientation-normalized full image.

```cpp
const sipi::LocalFileSource source(path);
const auto metadata = sipi::read_tiff_metadata(source);
const auto& image = std::get<sipi::iiif::ImageRequest>(parsed_request);
const sipi::iiif::SizePolicy policy{
    .allow_upscaling = config.size_upscaling
};
const auto result = sipi::iiif::resolve_geometry(
    image.region, image.size,
    metadata.image.display_width(), metadata.image.display_height(), policy);
```

The result describes a clipped integer crop and the requested output dimensions
**before rotation**. No parser objects are mutated and no codec reduction level
is chosen. Mirroring/rotation and rendering will consume this result later.

Rules follow [IIIF Image API 3 sections 4.1 and 4.2](https://iiif.io/api/image/3.0/):

- `full`, centered `square`, pixel and percentage regions are resolved. Origins
  outside the image fail; extents crossing an edge are clipped. Clipping uses
  subtraction rather than potentially overflowing `x + width` arithmetic.
- Percentage origins round down and percentage extents round up independently;
  odd centered squares place the extra pixel on the right/bottom. Rounding is an
  explicit server choice permitted by section 4.7.
- Width-only, height-only, percentage, and confined sizes preserve aspect ratio
  approximately. Exact `w,h` may distort. Output dimensions round to nearest
  integer, ties upward; results rounding below 1 or beyond uint32_t fail.
- `!w,h` fits within the box, original region, and policy limits. An oversized
  bounding box is valid without `^` because this form never requires enlargement.
- An exact request requiring enlargement without `^` fails. With `^`, actual
  enlargement requires `SizePolicy::allow_upscaling`. A prefixed request that
  shrinks or retains size remains valid even if enlargement is disabled.
- `max` and confined sizes adapt to optional width/height/area limits. Other forms
  exceeding limits fail instead of silently changing the requested dimensions.
  Adaptive area-boundary rounding falls back to floor if nearest rounding would
  cross the limit. Limits apply before rotation; rotated raster budgeting remains
  future work.
- `max_height` requires `max_width`; when omitted it equals `max_width`. All
  specified limits must be positive. Area multiplication uses uint64_t.
- `^max` needs at least one explicit finite width or area limit to have a defined
  enlargement target. Without one the result is `unbounded_maximum`; no arbitrary
  unlimited allocation is inferred. Limits are API policy inputs at present, not
  additional startup configuration fields. Defining those deployment limits is
  required before full `^max` service support.

`upscaling_disabled` is distinct so the HTTP adapter can map it to IIIF's 501.
Invalid region/size and exceeded limits are client errors (400). Invalid image
metadata or policy is a server-side concern, not a request-syntax error.
Geometry success alone does not confirm codec, quality, rotation, or file access
support.

## Configuration

The only new startup setting is `size-upscaling`, default `false`:

```sh
./build/Debug/sipi-2 --size-upscaling true
SIPI_SIZE_UPSCALING=true ./build/Debug/sipi-2
```

```toml
size-upscaling = true
```

Only lowercase `true` and `false` are accepted, with the usual CLI > environment
> file > default precedence. `ServerConfig::size_upscaling` supplies the geometry
policy as shown above. The HTTP handler applies metadata and geometry validation;
this option permits enlargement in geometry checks but cannot yet enable enlargement rendering. Reductions are implemented separately.
Actual output currently requires the resolved size to equal the crop dimensions. See [source resolution](source-resolution.md).

## Feature target

The owner's target is all features below, with only `sizeUpscaling` switchable.
This table is a roadmap, not an `info.json` capability advertisement.

| Features | Current state / next integration |
| --- | --- |
| `regionByPct`, `regionByPx`, `regionSquare` | Implemented for supported tiled RGB8 TIFF crops; no capability advertisement yet |
| `sizeByConfinedWh`, `sizeByH`, `sizeByPct`, `sizeByW`, `sizeByWh` | Syntax and geometry implemented; rendering pending |
| `sizeUpscaling` | Startup option and geometry policy implemented; rendering and finite `^max` deployment limits pending |
| `mirroring`, `rotationArbitrary`, `rotationBy90s` | Syntax implemented; pixel transformations pending |
| `baseUriRedirect` | Pending identifier/resource resolution and routing disambiguation |
| `canonicalLinkHeader`, `profileLinkHeader` | Pending image responses and canonical URI generation |
| `cors` | Pending consistent headers including errors and suitable OPTIONS handling |
| `jsonldMediaType` | Information JSON implemented; JSON-LD media type/content negotiation pending |

Advertise capabilities only after the complete behavior is implemented. Supporting
all named features does not imply support for every image format/codec.

## Tests

CTest covers all region/size forms, clipping, rounding, overflow, explicit limits,
caret/policy combinations, and confined-size invariants. TIFF tests generate small
classic TIFF/BigTIFF fixtures with strips, tiles, all eight orientations, ICC and
alpha, main-directory pyramids, SubIFD arrays, and unrelated pages. Intentionally
invalid compressed pixel payload verifies that metadata reading does not decode
pixels. Header failures and independent concurrent reads are also exercised.
A composition test combines startup configuration, a real TIFF header, parsing,
and geometry without inventing a URL-to-filesystem mapping.

## First pixel reader: tiled RGB8

`read_tiff_region(source, region, max_decoded_bytes)` in `TiffReader.h` decodes the
first image directory at its full stored resolution. Supported inputs are classic
TIFF or BigTIFF with tiles, unsigned 8-bit RGB, three contiguous samples, no extra
samples, and top-left orientation. Other layouts raise `TiffReadError` explicitly.
No silent conversion of alpha, bit depth, color model, or orientation is performed.

`PixelRegion` now lives in `src/image/PixelRegion.h`; the IIIF namespace retains an
alias for source compatibility. The decoder includes no IIIF parser header.
`RgbImage8` owns a tightly packed row-major RGB vector and the source ICC bytes.
RGB samples are not implicitly sRGB. Profile validation and conversion are later
work. Both types are usable by another format reader without TIFF or IIIF types.

```cpp
const sipi::LocalFileSource source(path);
const auto metadata = sipi::read_tiff_metadata(source);
const auto resolved = sipi::iiif::resolve_geometry(
    request.region, request.size,
    metadata.image.display_width(), metadata.image.display_height(), policy);
if (const auto* geometry = std::get_if<sipi::iiif::ImageGeometry>(&resolved)) {
    auto crop = sipi::read_tiff_region(source, geometry->region);
    // crop.width/height describe the source crop. Resizing to output_width/height
    // has not happened; this step only reads pixels.
}
```

The rectangle must be nonempty and entirely inside the current image. Geometry
resolution performs clipping; the decoder rejects invalid bounds rather than
clipping a second time. Only intersecting tiles are decoded, into one reusable
buffer. Each tile intersection is computed in full-image coordinates and then
translated separately into tile and output-buffer coordinates. Padding outside
edge tiles never becomes part of the returned raster.

The combined output-plus-tile allocation is bounded by the supplied byte limit
(default 256 MiB). Products are checked before multiplication/allocation. libtiff
and metadata limits are separate. Tile errors or short reads throw before any
partial image is returned. Reuse the same open source for metadata
planning and decoding. Local atomic replacement preserves this opened file;
in-place writes remain prohibited.

Metadata and region reads share opening, diagnostics, tag/profile reading, and
RAII code in `TiffReader.cpp`. The CMake library is now named `tiffreader` (formerly
`tiffmetadata`); the exception was renamed from `TiffMetadataError` to
`TiffReadError` to include pixel failures. Exceptions indicate file/codec failures;
invalid IIIF geometry still uses the existing result variant.

Tests compare each output pixel with independent coordinate-based RGB patterns,
using non-square tiles and partial right/bottom tiles, boundary crossings, full
images, one-pixel crops, and deterministic varied crops. Uncompressed, LZW,
Deflate, PackBits, and BigTIFF cases pass. A corrupt nonintersecting tile does not
prevent a crop elsewhere, but fails a crop touching it. Tests also check unsupported
layouts, invalid regions, allocation boundaries, ICC ownership, and the complete
metadata -> parser -> geometry -> pixel-crop path.

JPEG encoding and HTTP delivery are integrated. Pyramid selection, orientation
transforms, other pixel layouts, and enlargement remain subsequent increments.
Area-weighted RGB8 reduction is integrated after crop reading; see [resizing](area-resize.md). The
byte-source boundary supports the tested S3 adapter without changing TIFF APIs.
Evaluate Grok before generalizing the pixel model beyond concrete requirements.

## Pure TIFF read planning

`src/imgformats/TiffReadPlan.h/.cpp` provides `plan_tiff_read(metadata, geometry)`.
It performs no I/O; the HTTP worker now uses its result for decoder selection. Its
inputs are the compatible directories produced by `read_tiff_metadata` and the
resolved `iiif::ImageGeometry`. The returned value owns a copy of the selected
`TiffDirectory`; it must be used with the same source version as the metadata.

Selection uses the smallest directory area for which the mapped crop has at
least `output_width` and `output_height` pixels, checked separately using exact
integer cross-products. Directory order need not be sorted; equal-area candidates
retain their input order. If no reduced level suffices, use the main image. If
the request itself needs enlargement, the plan still retains the main image;
upscaling policy and rendering support remain separate checks.

Coordinates refer to pixel edges in orientation-normalized space. For each axis,
map full-resolution edges by `level_dimension / base_dimension`. Floor the left
edge and ceil the right edge to obtain the smallest enclosing integer
`read_region`. The exact interval inside that rectangle is retained as:

- `offset_numerator / denominator`: offset from the read origin;
- `extent_numerator / denominator`: requested extent;
- `denominator`: full-resolution dimension of this axis.

For example, a horizontal crop `x=3, width=503` in a 1001-pixel image maps into a
251-pixel level with integer read interval `[0,127)`, offset `753/1001` and extent
`126253/1001`. It can supply 126 output pixels without enlargement, but not 127.
The extra edge pixels belong to the enclosure, not to the requested crop.
Different ratios on the two axes correctly handle rounded odd pyramid dimensions.
Validated 32-bit dimensions and 64-bit products avoid floating-point selection
errors; ceiling uses quotient/remainder rather than potentially overflowing sums.

This is a geometric plan, not a decoder-capability check. It can describe strip
layouts and all metadata orientations without claiming that the current pixel
reader can decode them. The worker checks base support and removes unsupported
levels before planning. `read_tiff_region(source, directory, region, budget)`
selects the planned IFD by offset, rechecks actual tags and validates planned
dimensions/orientation. It retains the main ICC profile when reduced IFDs omit it.
Top-left RGB8 remains the supported pixel subset, so no orientation translation
is needed yet. The fractional-window overload of `downsample_area` then resamples
the **exact fractional window** using the format-neutral `PixelWindow` type.
Scaling the whole enclosing rectangle would shift/expand the requested view.
No interpolation halo is included; a later filter needing neighboring pixels
must explicitly extend its read region.

`tests/TiffReadPlanTests.cpp` covers odd pyramids, exact thresholds in both axes,
fractional crops, unordered levels, base fallback, all orientation dimensions,
exhaustive small rectangles and products near the uint32 limit. These tests need
no image file, local repository or S3 connection.

`tests/TiffPyramidTests.cpp` exercises actual decoding of main-chain and BigTIFF
SubIFD pyramids, odd dimensions, fractional resampling, inherited ICC, skipped
unsupported layouts, selected-directory validation, tile isolation and memory
limits. A deliberately damaged base tile proves reduced delivery does not decode
the main image. HTTP tests compare reduced-IFD output with a flat reference and
check GET/HEAD; MinIO tests compare fractional pyramid delivery with local output.
