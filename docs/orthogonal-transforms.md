# Mirroring and quarter-turn rotations

The shared image pipeline supports horizontal mirroring and clockwise rotations
of 0, 90, 180, 270 and 360 degrees, for every currently supported TIFF/JPEG 2000
pixel layout and for JPEG/PNG output. Region selection and size reduction happen
first, then left-to-right mirroring, then rotation. This follows the
[IIIF Image API rotation order](https://iiif.io/api/image/3.0/#43-rotation).
The transformations permute complete pixels without interpolation or padding.

Examples (replace `image.tif` with an existing identifier):

| Request | Result |
| --- | --- |
| `/image.tif/full/max/!0/default.png` | Mirror horizontally |
| `/image.tif/full/300,200/90/default.jpg` | Resize to 300 x 200, then rotate clockwise; output 200 x 300 |
| `/image.tif/full/max/!270/default.png` | Mirror first, then rotate 270 degrees clockwise |

Existing size/upscaling restrictions still apply. Angles 0 and 360 are identical;
90.0 and 90 are equivalent. Other angles, including values merely close to 90,
now use [bilinear arbitrary rotation](arbitrary-rotation.md); they are never snapped
to the exact quarter-turn path.

## Owning raster API

`src/image/OrthogonalTransform.h/.cpp` belongs to `imageprocessing`. Its
`transform_orthogonal(RasterImage8, bool, QuarterTurn, max_working_bytes)` function
consumes an owning raster and returns the transformed raster. Pass `std::move`
when relinquishing ownership. The enum expresses supported operations without
coupling the processor to IIIF syntax or codecs; ImageTransform dispatches exact parsed angles here.
There is no shared mutable state.

Gray/gray-alpha/RGB/straight-RGBA8 channels are moved together. Layout and ICC profile bytes
are preserved, as are hidden RGB values in fully transparent pixels. JPEG's
existing white-background alpha composition follows the transform; PNG retains
alpha. This stage performs no color conversion.

Identity, mirroring and 180 degrees reuse the input pixel storage. For 90/270,
one equally sized output buffer is allocated, and any requested mirroring is
combined into the same pass. Width and height swap. The default working budget is
256 MiB, accounting for input pixels, ICC bytes and additional output pixels;
vector spare capacity and allocator overhead are excluded. Invalid input or a
budget excess throws `TransformError` before pixel mutation/output allocation.
Ordinary allocation failures can propagate. This is a per-operation budget, not
a whole-server RSS limit.

## HTTP and cache integration

Both local and S3 sources use the same post-decode pipeline. Efficient tile and
pyramid/resolution selection remains based on the pre-rotation crop and size.

Cache identity already contains mirroring and normalized rotation. Its geometry
remains pre-rotation, while indexed result dimensions describe the final encoded
image, including the width/height swap. Authorization, source version checks and
capability checks still precede result lookup. Existing identity output is
unchanged, so this addition does not require a rendering-recipe version bump.

`info.json` advertises `mirroring`, `rotationBy90s` and `rotationArbitrary` only for supported pixel
layouts. Metadata-readable unsupported layouts advertise neither. The existing
explicit development boundary (omitted mandatory `profile`) remains in place;
see [image information](iiif-info.md).

## Verification

`orthogonal-transform` checks all eight permutations against explicit asymmetric
pixel goldens for gray/RGB/RGBA, ICC preservation, degenerate dimensions, inverse
properties, input validity and exact memory thresholds. `rotation-http` compares
decoded PNGs with an independent row/transpose oracle after crop/reduction across
TIFF layouts/pyramids and JPEG 2000, checks JPEG dimensions, GET/HEAD, angle aliases,
cache metadata/hits/restart, authorization and arbitrary-angle interpolation. The
MinIO integration test also compares rotated local/S3 output.
