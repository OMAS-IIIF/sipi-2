# TIFF strips and native 8-bit grayscale

Delivery supports top-left unsigned 8-bit contiguous RGB, explicitly tagged RGBA,
and single-channel grayscale in Classic TIFF and BigTIFF. Both tiles and strips
work through the same local/S3 ByteSource and pyramid selection pipeline.
Pyramidal tiled TIFF remains recommended, especially for remote region access.

## Strip selection and memory

`read_tiff_region` decodes only strips intersecting the requested rectangle.
Each selected strip spans the full source width; only intersecting pixels are
copied into the output raster. Tile and strip decoding share rectangle-copy
logic, with one reusable decoded block buffer. The final strip may be shorter.
A source stored in a single strip still requires decoding that entire strip,
even for a tiny crop. Strip support does not imply tile-like access efficiency.

The existing 256 MiB decode budget covers the output raster plus one decoded
block. Metadata and codec allocations retain their separate limits. Short reads
and reported codec errors fail the request; no incomplete result is published.
LibTIFF can repair some malformed directory fields, so decoding is not a strict
repository validation service. Supported compression also requires a configured
libtiff codec; tests cover uncompressed, LZW, Deflate and PackBits data.

## Grayscale semantics

One unsigned 8-bit sample with MINISBLACK or MINISWHITE and no extra samples is
accepted. MINISWHITE values are inverted after decoding. `PixelLayout::gray`
therefore always means one byte per pixel, with 0 black and 255 white. No expansion
to three identical RGB channels is needed. Area reduction retains this layout and
averages stored values; it does not imply linear-light processing.

ICC bytes remain attached to their original color model. PNG uses grayscale color
type 0; JPEG uses one grayscale component. Both require a basic GRAY ICC header
for tagged grayscale input. These checks are not full profile validation or color
conversion. Untagged sources remain untagged.

For native grayscale, `default`, `color` and `gray` IIIF qualities all return the
grayscale image. Information responses advertise `extraQualities: ["gray", "bitonal"]` when
the source passes the metadata-level decode capability check. The existing
omission of `profile` remains until compliance is reviewed. RGB-to-gray conversion and
bitonal output follow the shared [color policy](color-management.md).

CTest generates these fixtures under the HTTP test image directory:

- `/gray.tif/full/max/0/default.png`: strip-based MINISBLACK.
- `/gray-white.tif/2,3,19,13/10,7/0/gray.png`: MINISWHITE crop and reduction.
- `/gray-tiled.tif/full/max/0/default.jpg`: native grayscale JPEG.
- `/strip-rgba.tif/full/max/0/default.png`: strip-based transparency.

## Boundaries and verification

Gray+alpha, 1-bit/16-bit/floating-point samples, palette/CMYK/Lab, separate planes,
and other orientations remain unsupported for delivery. ICC conversion is not
part of this increment. Upload conversion remains separate.

`tiff-strip-gray` checks polarity, strip boundaries, shortened final strips,
selective corruption handling, exact memory budgets, BigTIFF, compression,
pyramids and grayscale resizing. Codec tests inspect native grayscale encoding
and retained ICC. HTTP tests compare exact PNG pixels and decoded JPEG output;
MinIO tests compare remote and local strip/grayscale delivery.
