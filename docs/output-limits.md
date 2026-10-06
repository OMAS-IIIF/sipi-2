# Resource admission and output limits

Target: IIIF Image API **3.0 Level 2**, plus selected optional features. There is
no Image API 3 Level 3. This step establishes delivery policy; it does not publish
a profile or complete the separate HTTP/CORS/JSON-LD work.

## Public configuration

| CLI | Environment | TOML | Default | Accepted range |
| --- | --- | --- | --- | --- |
| --max-width | SIPI_MAX_WIDTH | max-width | 8192 | 1..32768 |
| --max-height | SIPI_MAX_HEIGHT | max-height | resolved max-width | 1..32768 |
| --max-area | SIPI_MAX_AREA | max-area | 8388608 | 1..8388608 |

Precedence is CLI > environment > explicitly selected TOML > default, per field.
An explicit height from any selected source is retained; only absent height
inherits the final width. Empty, zero, negative, fractional, overflow and values
above the current implementation ceilings are rejected. Help/version skip
environment/file loading as with other options.

Example:

    max-width = 4000
    max-height = 3000
    max-area = 8000000

The internal OutputLimits height value 0 denotes absence, never a user-selectable
unlimited setting. The current buffered pipeline has finite technical ceilings.

## Shared information/request contract

Every successful info.json reports the original full-image width/height and the
effective maxWidth, maxHeight and maxArea. These output limits do not reduce the
source coordinate system and are not a substitute for future access-restricted
virtual-image geometry. Preflight/source selection and authorization still run first.

The same SizePolicy drives information and request validation:

- max selects the largest non-upscaled rendition under all three limits.
- !w,h fits within the requested box and policy without enlargement.
- Explicit sizes exceeding policy return 400; no silent resizing.
- Adaptive max/fit retain at least one pixel on each axis, with area rechecked.
  Thus a 70000 × 1 source can produce full/max at 8192 × 1.
- Upscaling remains unimplemented in the renderer even when the geometry option
  permits it. These settings do not advertise sizeUpscaling.

Limits apply to the scaled region before rotation. The actual rotated canvas must
also fit the encoder and current 8388608-pixel technical ceiling. Some arbitrary
rotations of large/thin outputs therefore return 400. That check happens before
pixel decoding and cache lookup, not as a late encoding error.

Geometry and resource checks precede result-cache hits. Lowering a limit after
restart cannot resurrect a previously cached oversized answer. The cache key
already contains resolved crop/output size: no extra policy key or recipe bump
is needed when identical permitted geometry still produces identical pixels.
The one-pixel adaptive correction only enables previously rejected geometries.

## Admission without decoding

The worker rejects unsupported pixel layouts/color encodings with 501
source_not_deliverable on both info and image requests. This includes unsupported
TIFF orientations and metadata-readable but unsupported J2K variants. Validating
metadata alone does not make a resource an IIIF image service.

For supported layouts, the worker plans full/max and checks its known working
sets before admitting the resource. A full/max plan that cannot fit returns 501
source_not_deliverable for info and images, including smaller crops. This is an
intentional resource policy: the current server must be able to provide its
default full-image rendition. Large masters are permitted when a suitable
pyramid/resolution makes the read affordable. Large nonpyramidal sources may
need ingestion conversion or a future streaming reader.

Each image request then checks its own read plan, resize/rotation and processing
working sets. A deterministic request budget excess returns 400
request_exceeds_working_limits. No full raster is allocated for these checks.

The small module src/http/DeliveryPolicy.* owns this HTTP admission policy:

- TIFF: supported-level selection shared with actual decode; enclosing raster
  plus an intersecting tile/strip buffer. A strip spans the full source width.
- J2K: reduced raster plus the same conservative reduced int32 tile-sample
  estimate used by Grok delivery. Tiny ROI does not imply tiny codec tile storage.
- JPEG: complete raster plus a header-derived multiscan/progressive coefficient
  estimate. JpegMetadata retains this fact, rechecked during actual decode.
- PNG: complete expanded raster, including palette/transparency expansion.
- Processing: conservative four-channel resize/rotation buffers, source ICC,
  gray conversion and two dithering rows against the existing 256 MiB budget.
- Output: actual JPEG dimension/ICC marker bounds exported by JpegWriter and a
  common pixel ceiling, also for PNG.

Known limits and decoder guards must evolve together. These calculations do not
replace the defensive checks inside the decoders.

## Why these ceilings?

8192 is the default per-axis policy; area normally constrains square outputs
more tightly (approximately 2896 × 2896). Configuration can raise axes to 32768
but not bypass the 8388608-pixel area ceiling.

At that canvas ceiling, four-channel raw output is 32 MiB. Together with at most
16 MiB ICC, PNG row/filter and deflate overhead retain margin below the existing
64 MiB encoded-response cap. JPEG's common profile limit is slightly smaller
than 16 MiB and is checked separately. Gray/alpha conversion and row dithering
fit their stage budgets once the decoder/resampler working set also passes.
The rotated canvas gets the same area ceiling. These are conservative operational
limits for this implementation, not limits imposed by IIIF.

They are not a global RAM/RSS guarantee: retained metadata, encoded responses,
allocator capacity, concurrent workers and native codec/LCMS scratch add memory.
Compressed-stream read limits, corrupt blocks, semantic ICC failures, tile-header
overrides and late metadata may still fail during delivery. Admission deliberately
does not decode the whole image or construct a color transform on each info call.
Unexpected/corrupt-source failures retain their existing 500/reader error mapping.

## Verification

Configuration tests cover all precedence sources, inherited height, help/version,
duplicates and invalid/over-ceiling values. Pure planning tests cover a huge TIFF
with/without a usable pyramid, large strip/J2K tile buffers, JPEG coefficient
storage, PNG full decode and expanded rotation canvas. HTTP tests cover both
endpoints and HEAD, actual wide JPEG output, advertised limits, exact/fit/crop
geometry, source rejection and a cache filled before limits are lowered.

The focused conformance audit now passes its resource-admission and wide-JPEG
checks. HTTP/profile findings were subsequently addressed; see [HTTP policy](iiif-http.md).
