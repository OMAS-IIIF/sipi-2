# Color declarations, profiles and conversions

This is the technical basis for the planned GLAM color-handling handbook. It records
what SIPI actually does, including assumptions and limitations. Serving a derivative
never changes the original repository object or its metadata.

## The request decides whether to convert

`default` and `color` preserve the supported source color encoding. Choosing JPEG
alone does **not** convert a source to sRGB. `gray` now converts RGB/RGBA through
LittleCMS to **Gray Gamma 2.2 with a D50 white point**, with the resulting ICC profile
embedded in either PNG or JPEG. Existing grayscale stays grayscale without a new
conversion or profile assignment. `bitonal` builds on the same grayscale policy,
then composites on white and applies a fixed threshold (see below).

| Source | `default` / `color` | `gray` |
| --- | --- | --- |
| RGB/RGBA with usable RGB ICC | Preserve original encoding and ICC | Convert from that ICC to Gray Gamma 2.2/D50 |
| RGB/RGBA explicitly tagged sRGB | Preserve sRGB declaration/encoding | Convert from sRGB to Gray Gamma 2.2/D50 |
| RGB/RGBA with no retained color declaration/profile | Leave untagged | Assume sRGB **for this conversion**, then convert to Gray Gamma 2.2/D50 |
| Native gray/gray-alpha with ICC | Preserve values and original ICC | Preserve values and original ICC, even if not Gamma 2.2 |
| Native gray/gray-alpha with explicit sRGB | Preserve values and declaration | Preserve values and declaration |
| Native untagged gray/gray-alpha | Remain untagged | Remain untagged; do not assign Gamma 2.2 to existing values |
| PNG gAMA/cHRM without ICC or explicit sRGB | Pixel delivery remains 501 | Remains 501; do not override a known unsupported declaration |
| Unusable/non-RGB ICC on an RGB conversion input | Existing reader/writer validation applies | Fail with 500 color_conversion_failed; **never fall back to sRGB** |
| CICP/HDR, unsupported precision/orientation/animation | Existing subset rejection | Same rejection |

The untagged-sRGB rule is an explicit operational assumption, not recovered archival
knowledge. An untagged Adobe RGB or other non-sRGB source will be interpreted
incorrectly. Repositories should retain authoritative source profiles during ingest.
A debug message records the assumption when an actual RGB-to-gray conversion uses it.
A cache hit reuses the already-rendered derivative and does not repeat the conversion.

The fallback uses the color information retained by current readers: ICC and explicit
PNG sRGB. It does not infer color spaces from filenames or JPEG EXIF ColorSpace tags;
the JPEG EXIF reader still inspects orientation only. Known unsupported PNG declarations
are rejected before reaching the converter. See each input-format document for the
precise metadata-reading boundary.

## Conversion policy and stage order

The pipeline is: decode selected source region/resolution, crop/resize, mirror/rotate,
apply requested quality (`gray` or `bitonal`), then retain remaining alpha for PNG or composite on white and
encode JPEG. Conversion therefore operates on the requested output raster, not an
unnecessarily large original. Sampling before conversion remains in stored sample space.

`src/image/GrayConversion.*` uses pinned **LittleCMS 2.19.1** with:

- embedded RGB ICC, or an sRGB input profile for explicit/assumed sRGB;
- D50 gray output, pure power-law gamma 2.2, ICC version 4.3;
- fixed relative-colorimetric intent with black-point compensation;
- unchanged alpha coverage, copied independently of the color transform.

The fixed conversion intent is a server policy for this derivative, not inherited
from a source PNG sRGB intent. Relative colorimetry maps reference white; black-point
compensation accommodates different source/target black points. This is not a
perceptual gamut-mapping option exposed to clients.

The target ICC is named `sipi-2 Gray Gamma 2.2 D50`. Its creation date is fixed to
2026-01-01 and its optional profile ID is unset, avoiding time-dependent output/cache
bytes. The output carries the target ICC and no source sRGB declaration. Gray Gamma
2.2 differs from the piecewise sRGB transfer curve; it is not our existing sGrey profile.

Each conversion owns its LittleCMS context, profiles, tone curve and transform.
There is no global error-handler mutation or shared mutable transform. Native gray
passes through without a second buffer. The 256 MiB default conversion budget counts
input/output raster bytes and source/target ICC; native LittleCMS allocations and
allocator capacity/overhead are additional. RGB source ICC is limited to 16 MiB.

## Bitonal quality

`bitonal` first follows exactly the `gray` column in the table above, including
source-profile validation and the untagged-RGB assumption. After resizing and rotation,
it composites gray-alpha onto **white**, then maps stored values **below 128 to 0**
and **128 or above to 255** by default (`bitonal-method=threshold`).
Optional Floyd–Steinberg dithering is described below; adaptive thresholding is not implemented.
The result is opaque: transparent rotation borders become white, and partially
transparent edges are composited before thresholding so they cannot remain gray.

The threshold is an encoded sample midpoint, **not 50% linear luminance**. Native
gray retains its original encoding/profile, even if its gamma differs from 2.2;
RGB-derived gray uses the Gray Gamma 2.2/D50 conversion described above. Profiles
describe the resulting black/white endpoints and are retained, not invented for
untagged native gray. White compositing uses rounded integer sample arithmetic,
as in existing JPEG alpha handling, rather than linear-light compositing.

`src/image/Bitonal.*` consumes gray or gray-alpha, reuses its storage, and produces
opaque gray8. It deliberately does not accept RGB; the worker composes it with
`convert_to_gray`. Validation and the default 256 MiB pixel/profile budget precede
mutation. The method is selected at startup, as described below.

Use `/identifier/full/max/0/bitonal.png` for **exactly two sample values**.
The PNG encoder now writes packed **1-bit grayscale**, for either rendering
method. Decoding expands bits back to exactly 0 and 255. Internal processing
remains gray8; packing introduces no color conversion or pixel change.
`bitonal.jpg` starts from the same binary raster, but lossy JPEG compression can
introduce intermediate decoded gray values and edge artifacts. Prefer PNG for
documents requiring exact black/white output. The original repository image is unchanged.

### Optional Floyd–Steinberg dithering

Select `--bitonal-method=floyd-steinberg`, `SIPI_BITONAL_METHOD=floyd-steinberg`,
or TOML `bitonal-method = "floyd-steinberg"`. Priority remains CLI > environment >
TOML > default (`threshold`). This affects only IIIF `bitonal`; the request URL
and advertised quality remain unchanged. Restart the server to change the policy.

After gray conversion and white compositing, Floyd–Steinberg diffuses quantization
error with weights 7/16 ahead, 3/16 below-behind, 5/16 below, 1/16 below-ahead.
Even rows scan left-to-right and odd rows right-to-left, starting at the output
raster origin. See [the algorithm and serpentine scanning](https://tannerhelland.com/2012/12/28/dithering-eleven-algorithms-source-code.html).
The threshold remains 128; double-precision error is not rounded or clamped
between pixels. Off-image error is discarded without renormalizing weights.
Two padded error rows require `2 * (width + 2) * sizeof(double)` bytes, counted
alongside input pixels and ICC in the existing 256 MiB budget. Each request owns
its error rows: no shared state or additional full-image buffer is required.

Dithering preserves local average **encoded sample values**, not linear luminance.
The existing gamma/profile policy is unchanged; this is not calibrated print
halftoning. It can retain tonal detail in photographs, while fixed thresholding
often suits text and line drawings better. Both methods produce opaque binary
samples; PNG remains the recommended exact output format.

**IIIF tile limitation:** diffusion starts independently for every requested
output raster, after crop/scale/rotation. A separately rendered crop or tile can
differ from the same rectangle in a full-image rendering. Adjacent tiles may show
pattern discontinuities. Full-image context or a coordinate-anchored ordered
dither would require a separate design; this implementation does not promise
seamless tiled halftoning.

The persistent cache adds `bitonal.method=floyd-steinberg` only for dithered
bitonal output. An absent field retains the original threshold meaning, preserving
existing threshold and other-quality keys. Changing the setting cannot reuse the
other method's results. Reverse proxies/browser caches keyed only by URL need
their own invalidation when an administrator changes rendering configuration.

## Profile preservation versus assignment

For `default`/`color`, existing ICC bytes remain unchanged. Explicit PNG sRGB is stored
as `srgb_intent` (0..3), mutually exclusive with ICC. PNG output writes the native sRGB
chunk; JPEG assigns a matching embedded [sRGB or sGrey ICC](../third_party/compact-icc/README.md).
That assignment describes already-sRGB values and performs **no pixel conversion**.
Gray/gray-alpha uses a gray profile after JPEG white composition; RGB/RGBA uses RGB.

For supported PNG metadata, ICC takes precedence over sRGB, then gAMA/cHRM, following
the [PNG color-chunk priority](https://www.w3.org/TR/png-3/#4Concepts.ColourSpaces).
CICP/HDR is explicitly rejected. Libpng warnings, malformed profiles and invalid
intent values remain fatal. Lower-priority declarations are not applied again.

Source sRGB intents (0 perceptual, 1 relative, 2 saturation, 3 absolute) are preserved
when no conversion is requested. During RGB-to-gray conversion, the fixed policy above
applies and the target ICC specifies relative colorimetry. Both writers reject a
raster carrying simultaneous ICC and explicit sRGB declarations.

## Other operations and limitations

- JPEG RGB/YCbCr representation changes and lossy quantization are codec operations,
  distinct from ICC profile conversion.
- PNG palette, low-bit gray and tRNS expansion is lossless. Gray transparency now
  stays native gray-alpha, including with a GRAY ICC; it no longer expands into RGB.
- Area resizing and bilinear rotation alpha-weight stored samples without linearizing
  the transfer curve. Rotation adds transparent borders and preserves color metadata.
- PNG preserves straight alpha except for opaque bitonal output. JPEG composites onto white in the current stored
  encoding (Gray Gamma 2.2 after a color-to-gray conversion), not in linear light.
- Exact crop/mirroring/quarter turns do not perform color conversion.

These are deliberate, documented rendering choices. Future linear-light processing,
new target profiles, configurable intents or general RGB-to-RGB conversion must be
separate changes with corresponding cache-recipe updates. Gamma/chromaticity-only
input and HDR remain future work; source orientation/animation/precision
restrictions still apply.

## Verification and cache compatibility

Unit tests check analytical neutral values for assumed/explicit sRGB and an embedded
linear-RGB profile, target GRAY/gamma/D50/intent tags, exact alpha preservation, native
gray identity, invalid/profile-mismatch errors, memory bounds and concurrent determinism.
HTTP tests cover PNG/JPEG target ICC equality, source-profile precedence, unchanged
native gray/gray-alpha, authorization, failure nonpublication, cache/restart and fresh
uncached process equality. MinIO results match local rendering byte-for-byte.

Rendering recipe is now `sipi-render-v3`: packed one-bit bitonal PNG changes encoded
bytes. Version 2 introduced native PNG gray-alpha and RGB-to-gray conversion.
Old entries remain on disk but cannot match v3 keys and are evicted normally. Metadata
cache identity remains source-version-bound. Packing introduces no additional CLI/env/TOML option; the existing bitonal-method setting remains.

Bitonal unit tests check the 127/128 boundary, alpha-before-threshold ordering,
storage reuse, profile retention, idempotence and invalid inputs/budgets. HTTP tests
derive expected binary pixels from gray delivery independently, including arbitrary
rotation, ICC and native-gray inputs, cache hits/restart, HEAD, authorization and
failure nonpublication; local/MinIO results must match. Bitonal already has a distinct
cache quality key, with an additional method field for Floyd–Steinberg. The initial threshold addition needed no recipe increment because those requests
previously returned 501. Packed PNG output now requires v3, even though decoded
pixels are unchanged. Changing
threshold, dithering or alpha policy later requires a rendering-recipe increment.
