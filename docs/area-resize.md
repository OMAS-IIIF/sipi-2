# Area-weighted RGB/RGBA8 reduction

`image/AreaResize.h` provides `downsample_area` in the independent
`imageprocessing` target. It knows neither IIIF nor TIFF, S3, HTTP or JPEG.
Both output dimensions must be positive and at most the corresponding input
size. Either axis may remain unchanged. Enlargement is rejected.

## Pixel semantics

Treat each input pixel as a unit square with a constant channel value. Each
output pixel averages every input square intersecting its source footprint,
weighted by overlap area. For a five-pixel row reduced to two pixels:

- First output: `(p0 + p1 + 0.5*p2) / 2.5`.
- Second output: `(0.5*p2 + p3 + p4) / 2.5`.

Integer reduction factors therefore produce ordinary block averages. Fractional
factors account for edge contributions directly; there is no enlargement followed
by reduction, and no quantized intermediate. Coordinates and overlap widths are
computed in a common integer grid using uint64_t; weighted sums use double.
Final channel values round once to the nearest integer, with ties rounded up,
and clamp defensively to 0..255. Single-pixel axes need no special interpolation
formula and never divide by target dimension minus one.

This is a box/area filter, not a claim of optimal reconstruction for every image.
It averages **stored RGB channel values**, not linear light. ICC bytes remain
unchanged and untagged input remains untagged; no sRGB transfer function is assumed.
For example, averaging channel values 0 and 255 yields 128. Profile-aware
linear-light processing is a separate color-management decision; it must not be
silently inferred from the presence of three channels. RGBA uses alpha-weighted colors; see [alpha delivery](alpha-delivery.md).

## Ownership and memory

The function takes an owning `RasterImage8` by value. Pass `std::move(image)` to
transfer ownership without copying the input. An identity resize returns the same
pixel allocation. A reduction allocates only its output raster; ICC storage moves
unchanged. There is no expanded raster, intermediate image or weight table.
Concurrent independent calls have no shared state.

Validate positive dimensions, exact RGB byte count and size arithmetic before
processing. The default 256 MiB working budget includes input plus output pixel
bytes and one ICC profile; identity needs input plus profile only. Counts exclude
vector capacity, allocator overhead and objects owned by the caller. TIFF decoding
and JPEG encoding retain separate stage limits; these are not a process-wide
memory ceiling. `ResizeError` reports invalid input/enlargement/budget failure;
allocation failures may propagate. Ownership is consumed even on failure.

## Delivery pipeline and examples

The HTTP worker resolves geometry, reads the crop, reduces it if needed, then
encodes JPEG or PNG. The same processing works with local and S3 ByteSources:

```text
/tiled.tif/full/160,/0/default.jpg
/tiled.tif/full/pct:25/0/default.jpg
/tiled.tif/50,30,100,80/37,29/0/default.jpg
/tiled.tif/full/1,1/0/default.jpg
```

Only reductions or unchanged dimensions are rendered. Enlargement remains 501
(not_implemented when policy permits it, upscaling_disabled when policy rejects
it); mixed shrink/enlarge requests are also unsupported. Rotation/mirroring and
other output qualities/formats retain existing restrictions. HEAD follows the
same processing path as GET. Resize failures map to generic 500 processing_failed.

The TIFF reader selects crop-intersecting tiles from the smallest supported
pyramid level sufficient for both output axes. If none qualifies, it uses the
main image. This can reduce source I/O and decoded memory for small outputs;
nonpyramidal sources still pay the full-resolution crop cost. Remote range
buffering remains separate work.

## Fractional source windows

The overload `downsample_area(image, PixelWindow, width, height, budget)` accepts
a format-neutral exact crop within the decoded raster. `PixelInterval` stores
an offset and extent numerator sharing a uint32 denominator. It excludes padding
pixels introduced by the enclosing integer read rectangle. Output axes must fit
the exact extent, not the enclosure. Invalid, empty, out-of-bounds or zero-denominator
windows fail before output allocation. A full-image window delegates to the original
algorithm, including allocation reuse for identity.

For a fractional window, each output boundary is computed by quotient/remainder
arithmetic, splitting the extent before multiplication. This avoids overflowing
96-bit intermediate products while retaining exact contributing-pixel decisions.
Boundary fractions are converted to double relative to each source pixel when
accumulating overlap weights. Quantization uses `floor(value + 0.5)`; floating-point
accumulation at exact half-integer ties may change the last byte by one. There is
still no intermediate raster or weight table. ICC, budgets and stored-channel
semantics are the same as for the full-raster overload.

## Verification

`area-resize` compares all reductions of small non-square RGB patterns against an
independent common-grid replication/counting oracle. It checks fractional and
integer ratios, unchanged and single-pixel axes, constants, rounding, profile
preservation, identity storage reuse, invalid rasters and memory-limit boundaries.
HTTP tests decode actual JPEG responses and compare against independently averaged
fixture gradients with tolerance for JPEG loss, checking size syntax and HEAD.
MinIO HTTP tests compare reduced local and remote JPEG bodies exactly. Relevant
resizing/encoding/worker/HTTP tests also run under ASan/UBSan; external codec/SDK
binaries remain uninstrumented.

Fractional-window tests use an independent common-grid cell-counting oracle,
check window shifts, invalid fractions, memory limits and products that would
overflow a direct numerator-times-output-index calculation. Pyramid integration
tests prove actual reduced-IFD use with intentionally damaged base pixels.
