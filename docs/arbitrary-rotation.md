# Arbitrary-angle rotation

`src/image/ImageTransform.h/.cpp` adds clockwise bilinear rotation to the shared
post-resize pipeline for all currently pixel-supported sources, locally or on S3.
The order is crop/size, horizontal mirror, rotation, then PNG encoding or white
alpha composition plus JPEG encoding. No profile conversion occurs.

## Geometry and sampling

Exact 0/90/180/270/360 degrees delegate to `transform_orthogonal`: existing bytes,
layouts, hidden colors and fast paths remain unchanged. No near-angle snapping is
performed. Other angles use the full pixel-edge rectangle's rotated bounds:

- width = ceil(abs(cos(angle))*width + abs(sin(angle))*height)
- height = ceil(abs(sin(angle))*width + abs(cos(angle))*height)

`rotated_size` is the single geometry function used both by the HTTP/cache path and
the raster implementation. Invalid dimensions, nonfinite angles, values outside
0..360 and uint32 overflow throw `TransformError`. Calculations use double precision;
no special tolerance is applied to non-quarter-turn bounding-box rounding.

The source and output centers coincide. Output pixel centers are mapped backward
into source coordinates, avoiding holes from forward splatting. Four neighbors
contribute separable bilinear weights. Mirroring changes source sample indices
before the rotation, without allocating a separate mirrored raster.

Samples outside the source are transparent black. Interpolation accumulates color
weighted by both interpolation weight and source alpha, then divides by accumulated
alpha to return straight alpha. This prevents invisible source colors from bleeding
into visible edges. Alpha and color round to the nearest byte; fully transparent
output has deterministic zero hidden color. This is bilinear point sampling, not
exact polygon-area coverage or an area-preserving resampling guarantee.

RGB/RGBA outputs RGBA. Gray/gray-alpha outputs gray-alpha, retaining a GRAY ICC
without falsely attaching it to RGB channels. PNG encodes gray-alpha natively;
JPEG composition compacts it to gray on white. The new internal gray-alpha layout
is also supported by the existing exact transforms and area resampler. PNG input now also preserves gray-alpha natively, including GRAY profiles.

ICC bytes and explicit sRGB intent are preserved. Filtering and white composition
operate in stored sample space, not linear light. See [color management](color-management.md).

## Resource limits and cache behavior

General rotation allocates one expanded output raster. Before allocation, the
256 MiB default working budget checks input + output pixels + ICC. Vector spare
capacity, allocator overhead and other stages' memory are additional. There is no
global RSS quota. Failures propagate as 500 processing_failed; allocation failures
retain the existing worker error handling. Never publish partial output/cache data.

Cache identity already includes the requested angle and mirroring; indexed response
dimensions now come from `rotated_size`. Source layout remains the pre-transform
layout in the key. Existing authorization/version checks apply before hits, including
after restart. No recipe bump: non-quarter angles had no successful results before,
and exact rotations/previous rendering bytes are unchanged. Any future change to
bilinear sampling, bounding-box rounding or background must update the recipe.

Supported sources advertise rotationArbitrary in info.json alongside mirroring and
rotationBy90s. The separately approved missing profile field remains unchanged.
Invalid IIIF angle syntax/range is still rejected by the parser.

## Verification

`image-transform` covers analytic 1x1/45-degree weights, transparent hidden-color
rejection, mirror order, quarter-turn equivalence, gray-alpha resampling/white
composition and profile preservation, exact budget boundaries and invalid inputs.
Existing orthogonal tests continue to verify the eight exact permutations.

`rotation-http` compares output against an independent matrix/tent-kernel reference
across gray/RGB/RGBA TIFF and J2K crops, validates JPEG/PNG/HEAD/cache dimensions,
restart, denial and all indexed PNG dimensions. PNG-source tests cover ICC and sRGB
including gray-alpha output; MinIO compares arbitrary-angle results byte-for-byte
with local results. Interpolation comparisons allow one byte for floating-point
rounding differences; cache/local-S3 comparisons require identical bytes.
