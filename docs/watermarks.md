# Single-color watermarks

## Agreed behavior

The watermark is anchored to the full image, not independently positioned on
HTTP responses or OpenSeadragon tiles. Start with one centered mark: its width is
a fraction of the image width, reduced further if necessary to fit the height.
Preserve the mask's aspect ratio subject to integer pixel rounding. Multicolor
logos and repeated patterns are outside this increment.

A mask contains 8-bit coverage: 0 is transparent, 255 is opaque. Binary masks
are a subset; scaling may create intermediate coverage for antialiased edges.
The chosen single color and global opacity are separate from the mask.
Coverage multiplied by opacity gives the source alpha.

## Implemented building block

`src/image/Watermark.h` and `.cpp` provide two codec-independent operations:

- `prepare_watermark`: consume or share an immutable untagged gray8 raster without resizing
  or allocating pixels. Store its centered rectangle (`x`, `y`, `width`, `height`)
  separately from the original mask dimensions and the full canvas dimensions. Round dimensions half up, minimum one pixel. With odd spare
  space, the extra pixel is on the right/bottom. ICC/sRGB-tagged masks are rejected:
  the caller must explicitly extract coverage rather than treat luminance as alpha.
- `apply_watermark`: consume a full image or an unscaled crop at an explicit
  full-image origin. Composite the overlapping mask with straight-alpha source-over.
  Gray, gray-alpha, RGB and RGBA are supported; invisible destination colors do not
  contaminate visible results. Zero source coverage leaves all bytes untouched.

The prepared value owns its mask and is read-only during application. Each call
owns and mutates only its destination. No file handles, global state, I/O, policy,
HTTP or cache dependency is introduced. Invalid dimensions, buffer sizes, options,
color channel counts and memory budgets fail before compositing modifies pixels.
The default working budget is 256 MiB; vector capacity/allocator overhead is not
included. Preparation accounts only for the original mask; application accounts
for image pixels/profile plus that same original mask. Canvas dimensions do not
increase the mask allocation. Allocation failures may propagate.

### PNG mask loading

`read_png_mask` in `src/imgformats/PngReader.*` borrows a version-bound `ByteSource`
and decodes in one pass through IEND using the existing libpng reader. This boundary
works with local or remote storage without reopening a pathname. It returns the
untagged gray8 raster accepted by `prepare_watermark`.

| PNG input | Mask behavior |
| --- | --- |
| Gray, 1/2/4/8 bits, no color declarations | Accepted; expand samples linearly to 0..255 |
| Adam7 interlacing | Accepted with the same sample values |
| RGB, RGBA, gray-alpha, palette, or tRNS | Rejected; no implicit channel extraction |
| ICC, sRGB, gAMA, cHRM | Rejected; mask samples represent coverage, not encoded luminance |
| 16-bit, EXIF, APNG, CICP/HDR | Rejected |
| Invalid CRC, truncation, malformed chunks, warnings | Fail without returning a partial mask |

Export a **grayscale PNG without alpha or a color profile/gamma declaration**.
Black means transparent, white opaque; this is not a black logo on a white
background and no automatic inversion occurs. A 1-bit mask expands 0/1 to 0/255;
2-bit values become 0/85/170/255. No gamma or profile conversion occurs. Metadata
restrictions also apply after IDAT, and animated chunks are rejected anywhere.
Unrelated ancillary text is discarded using the existing reader policy.

The defaults are 16 MiB for the decoded mask and 16 MiB for cumulative input reads,
including buffered prefetch. The pixel budget is checked before raster allocation.
Both limits are explicit function arguments; no server configuration is introduced
yet. Existing libpng limits remain: dimensions at most 1,000,000 per axis and native
per-chunk allocation at most 16 MiB. Codec scratch/ancillary metadata and allocator
overhead are additional; the pixel limit is not a process-wide memory quota.

Unsupported masks throw `PngUnsupportedError`, malformed data/limits throw
`PngReadError`, and storage exceptions propagate unchanged. Each call owns its
codec and buffers; parallel reads do not share mutable decoder state. The ordinary
image reader retains its existing broader PNG subset and color-preservation rules.

### Color contract

The low-level function takes one destination-encoded sample for gray or three for
RGB. It does not interpret those bytes as sRGB, assign a profile or convert colors.
Compositing occurs in stored sample space, consistently with existing image
operations, not linear light. The destination's ICC/sRGB declaration is preserved.
`watermark_color` in `src/image/WatermarkColor.*` now converts one configured
**sRGB8 RGB triplet** to one destination-gray or three destination-RGB samples.
Pass its result to `apply_watermark`; alpha/opacity and coverage remain separate.
Only this swatch is converted, not the source image:

| Destination | Conversion target |
| --- | --- |
| RGB/RGBA with usable RGB ICC | Embedded ICC |
| Gray/gray-alpha with usable GRAY ICC | Embedded ICC |
| Explicit sRGB RGB/RGBA without ICC | Identical sRGB samples; no transform needed |
| Explicit sRGB gray/gray-alpha without ICC | Existing sRGB gray profile (sRGB transfer curve, not gamma 2.2) |
| Untagged RGB/RGBA | Assume sRGB for the swatch; preserve the image's untagged state |
| Untagged gray/gray-alpha | Assume Gray Gamma 2.2/D50 for the swatch; preserve the image's untagged state |
| Invalid, mismatched or unusable ICC; conflicting declarations | Fail with ColorConversionError, never fall back |

LittleCMS uses relative colorimetry and black-point compensation, matching the
existing gray conversion policy. ICC header rendering intent does not override
this fixed transform policy. Device-link, abstract and named-color profiles are
rejected. A profile that cannot support the transform also fails. The image is
borrowed, validated and unchanged; its declaration, original pixels and alpha are
never assigned or rewritten by this function. Untagged assumptions do not recover
unknown archival color information. Tag repository images authoritatively at ingest.

Each call owns a LittleCMS context/profiles/transform with a local error callback.
ICC input is capped at 16 MiB. The working-byte check covers borrowed image/profile
bytes; tiny result/generated-profile allocations and native LCMS scratch are
additional. There is no global profile/transform cache. Sampling and compositing
still take place in stored sample space; this does not introduce linear-light
blending. Apply the swatch before later IIIF gray/bitonal conversion so those
operations affect the image and watermark together.

### Coordinates and reduced resolutions

The native overload uses the full-image grid. The `WatermarkGrid` overload takes
**complete level dimensions** and the decoded crop's origin within that level.
These values describe the enclosing decoded rectangle before the read plan's
fractional window is applied. TIFF uses `directory.display_width/height()` and
`read_region.x/y`; JPEG2000 uses `level_width/level_height` and
`reduced_region.x/y` from `J2kReadPlan`. The latter now exposes complete level
extents, including rounding at nonzero reference-grid origins. Do not substitute
`original_width / 2^reduction`, the crop dimensions or final IIIF output dimensions.

Each level pixel maps proportionally to the full canvas, then directly into the
**original** mask. Clipping against the positioned rectangle and subtracting its
origin use uint64 arithmetic before floating-point conversion, preserving location
at large coordinates. A separable filter uses area weights on shrinking axes and
linear interpolation on enlarged axes (bilinear if both enlarge). The filter choice
uses the complete pixel footprint. At a mask boundary, sample the clipped visible
footprint and multiply by its share of the pixel area; the rest is transparent.
Linear interpolation clamps to edge samples and uses the visible footprint center.

No full-size or reduced mask and no contribution table is allocated. Only the
original samples contributing to each requested pixel are visited. Keep coverage
in double until compositing. This direct filter deliberately replaces the earlier
resize-to-full-resolution/quantize/reduce sequence: individual values may differ,
not merely because of rounding but because the two filters are no longer cascaded.
There was no HTTP watermark output or cache recipe to invalidate.

Apply the mark before the existing fractional window resize, mirror/rotation and
requested quality conversion. Enlarged output is produced by that later resize,
not by passing an oversized decoder grid.

Independently processed crops agree byte-for-byte with the same pixels of a complete
level. Tests include odd, anisotropic level dimensions and actual Grok full/window
reconstructions followed by fractional cropping. Position/footprint agreement is
not a promise of identical colors between different resolutions: averaging a
source image and then compositing does not generally equal averaging the composited
image, particularly with varying colors/alpha. Existing pyramid filters differ too.

A coarse pixel can integrate many original-mask samples, but never samples from
an expanded canvas-sized intermediate. Preparation is independent of canvas area;
per-pixel work depends on the original mask and the requested footprint. Very large
original mask files still need explicit decode/memory limits. No throughput benchmark
or mask caching policy is introduced in this increment.

## HTTP delivery and cache identity

The [typed Lua decision](preflight.md#single-color-watermark-decision) now drives
real delivery. After preflight, the worker opens the image and required mask through
the same local/S3 source resolver. Relative local masks use `iiif-root`; explicit
S3 keys use the configured bucket. Image and mask may use different backends.
The mask is read and validated before info/base responses and before result-cache
lookup, including HEAD. Missing, unreadable or invalid required masks produce
`500 / watermark_failed`, without exposing private paths or returning an unmarked
fallback. Opacity zero still requires a valid mask. Color-conversion failures retain
the existing `color_conversion_failed` response.

On a render miss, the decoder supplies its full selected-level dimensions and the
read-window origin. The worker converts the configured sRGB swatch to the raster's
encoding, prepares full-image placement and applies coverage at that decoder grid.
Fractional crop/resize, mirror/rotation, quality conversion and JPEG/PNG/WebP encoding
then process image and mark together. TIFF pyramid selection and J2K reduced reads
remain active; no full-resolution image or expanded mask is needed for them.

Marked results use the `watermarked` variant plus an owning `WatermarkIdentity`:
mask source identity/version/length, exact normalized binary64 width and opacity,
sRGB8 color, and a versioned placement/filter/color recipe. Each changed setting or
mask version selects a different cache entry. Unmarked serialization is unchanged.
Authorization and current mask validation precede every cache hit, also after a
restart; an existing result never substitutes for a missing mask.

This single-color overlay leaves public dimensions and coordinates unchanged.
`info.json` and canonical links therefore retain the requested service identity.
As with policy-selected source files, the representation depends on current
preflight authorization; responses remain `Cache-Control: no-store`. Following a
canonical link performs authorization again. Future resolution-restricted virtual
images still require a separate decision about effective coordinates/service identity.

## Decoded-mask memory cache

`src/cache/MaskCache.*` implements a worker-pool-wide LRU, independent of the
persistent result cache. Defaults are 64 entries and 32 MiB; configure
`mask-cache-max-entries` and `mask-cache-max-bytes` through CLI/environment/TOML
with the usual priority. Either zero disables retention. No storage is allocated
up front, and restart starts with an empty cache.

Every authorized request still opens the selected mask first. Local open/fstat or
S3 HEAD provides current identity, version and length; all three form the key.
Only a hit for that exact opened source skips PNG reads and decoding. A miss uses
the existing strict loader (16 MiB encoded and decoded limits), then admits the
successful immutable gray8 result. Invalid/missing sources are never cached and
never fall back to earlier versions. Replace local files atomically as for repository
images; in-place concurrent writes are not a supported publication mechanism.
S3 opening still requires a network round trip even on a mask-cache hit.

Color, opacity, target size and image identity do not enter this cache key: they
change application of the mask, not its raw coverage samples. `PreparedWatermark`
shares `std::shared_ptr<const RasterImage8>` storage and adds request-specific
placement. Neither preparation nor another tile request copies the mask pixels.
An evicted value remains valid while an active request holds it.

One short mutex protects lookup/admission/LRU updates; file/network I/O, decoding
and compositing run outside it. Simultaneous cold misses may decode independently;
the first admitted value is retained. There is no global lock across source I/O.
The byte budget counts resident raster/vector capacities, key strings and list nodes.
Allocator/shared-pointer overhead, active decoder scratch and caller-retained evicted
values are additional: this is not a process-RSS ceiling. Admission failure or an
oversized entry still permits delivery with the successfully decoded request value.

Debug logs report mask cache hit/miss/admission declined without identifiers or
paths. No HTTP headers, policy decisions, color conversions or prepared rectangles
are stored here; result-cache identity and rendered pixels remain unchanged.

## Validation

`watermark-tests` covers relative sizing, height fit, odd centering, binary reduction,
bilinear enlargement, fractional coverage/opacity, all four layouts, straight alpha,
profile preservation, crop equivalence, unaffected pixels and invalid/budget inputs.
`watermark-grid-tests` additionally compares direct sampling against the independent
existing resizer (area, bilinear and mixed axes), partitions all levels of a 17x13 canvas into independent crops,
checks straight alpha, uint32-limit coordinates and invalid grids, and consumes
real TIFF/J2K planner output. `j2k-region-tests` now checks watermarked real decoded
crops against complete-level reconstructions after fractional resampling.
Run `ctest --test-dir build/Debug -R '^(watermark|watermark-grid|j2k-region|tiff-read-plan|area-resize|tiff-pyramid)$' --output-on-failure`.
Both watermark suites also pass ASan/UBSan. A UINT32_MAX-square virtual canvas
with a 2x2 checkerboard mask reduces to one pixel under a five-byte pixel budget;
a one-sample mask on that canvas supports native-resolution crops with two bytes.
These tests exercise coordinates and allocation bounds without allocating the canvas.

`png-mask-tests` uses independently generated PNG fixtures for all accepted bit
depths and both interlace modes, exact expansion/compositing, alpha/palette/color
metadata rejection, malformed input, byte/pixel budget boundaries, propagated
storage failures and concurrent reading. Run
`ctest --test-dir build/Debug -R '^(png-mask|png-metadata|watermark|watermark-grid)$' --output-on-failure`.
The PNG mask and existing PNG reader suites also pass ASan/UBSan.

`watermark-color-tests` checks sRGB identity, independent linear RGB/gray numeric
oracles, agreement with existing gray conversion, explicit gray sRGB, alpha/layout
behavior, destination immutability, invalid/profile-class/size/budget rejection and
parallel determinism. The new converter and existing gray conversion pass ASan/UBSan.

`watermark-preflight-http` verifies independent expected PNG pixels, crop consistency,
real TIFF/J2K native and reduced reads, JPEG/WebP output, mirror/rotation and quality
paths, info/base/HEAD, policy/cache separation, concurrency, mask replacement and
failure after a warm cache and restart. Cache-key tests protect both parameter
separation and the unchanged unmarked serialization.

The opt-in `s3-http` integration additionally exercises a remote mask with an
S3-backed image, warm-cache reuse, replacement by a different ETag, and deletion.
The new cache/HTTP path also passes AddressSanitizer and UndefinedBehaviorSanitizer.

`mask-cache` tests entry/byte eviction, exact source keys, disabled/oversized
admission, invalid masks and shared lifetime after eviction/destruction. HTTP tests
also prove one decode across different renders with the result cache disabled,
zero-entry environment wiring, concurrent requests and replacement/deletion while
both caches are warm. Configuration tests cover CLI/environment/TOML precedence.

## Performance measurements

[Release benchmarks, 2026-10-08](benchmarks/2026-10-08-watermarks.md) compare
local/MinIO, TIFF/JP2 and mask/result caches independently. They include longer
warm-hit phases, byte verification, observed gains and measurement limitations.

## Reduced public images

With a Lua resolution restriction, the complete reduced public raster is materialized first. Watermark position/size refer to that public canvas, then all IIIF operations run. The unmarked public raster can be shared; final result keys still bind the required mask version/settings. See [restricted resolution](restricted-resolution.md).
