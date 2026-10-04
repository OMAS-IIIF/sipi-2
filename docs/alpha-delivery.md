# RGBA transparency

Delivery accepts tiled or strip-based top-left unsigned RGB8 and RGBA8 TIFF/BigTIFF, including
compatible pyramid levels, from local or S3 storage. An RGBA directory must have
four contiguous samples and exactly one `ExtraSamples` entry declaring straight
(unassociated) or premultiplied (associated) alpha. Unspecified extra samples,
separate mask IFDs, other orientations and other pixel layouts remain unsupported.
Pyramid candidates must still match the main image's sample and alpha declaration.

## Internal representation and decoding

`RasterImage8` replaces `RgbImage8`. It owns dimensions, pixel and ICC vectors,
and `PixelLayout::rgb` or `PixelLayout::rgba`. All RGBA rasters store **straight
alpha**: RGB is independent of coverage, and alpha ranges from 0 to 255. The
layout determines three or four tightly packed channels; invalid layouts are
rejected at processing/encoding boundaries. No borrowed codec storage survives.

TIFF decoding reads only intersecting tiles of the selected IFD. Straight samples
are retained, including hidden RGB under zero alpha. Associated samples are
normalized after copying the requested crop:

```text
straight = round_half_up(premultiplied * 255 / alpha)
```

At zero alpha, RGB becomes zero. A requested associated sample whose color exceeds
alpha is invalid and fails the request; unused tile padding/outside-crop samples
are not interpreted. Division cannot recover information lost when 8-bit
premultiplied data was created. Decoding therefore need not reproduce the bytes
of a separately stored straight-alpha original. ICC comes from the main image.

## Alpha-aware area filtering

Filtering uses stored color values, without implicit sRGB or linear-light
conversion. For overlap weights `w`, straight colors `C`, and alpha bytes `A`:

```text
A_out = sum(w * A) / sum(w)
C_out = sum(w * A * C) / sum(w * A)
```

The accumulation uses double; no intermediate premultiplied byte raster is
allocated. Final values round to nearest (half up in floating arithmetic).
Output whose rounded alpha is zero gets black hidden RGB, avoiding division by
zero and providing deterministic transparent output. Identity returns the original
storage and bytes. Exact rational windows and existing no-enlargement rules apply.

For example, opaque red plus transparent blue reduces to red at approximately
half opacity, not purple. Alpha is coverage and never receives a color transfer
function. Future profile-aware linear-light filtering must be an explicit color
pipeline change; this implementation does not claim physically linear mixing.

## Output behavior

PNG writes straight RGBA as color type 6, preserving the processed samples and
accepted ICC bytes. JPEG cannot carry alpha, so the worker explicitly invokes
`composite_on_white` before the gray/RGB JPEG encoder:

```text
C_rgb = round_half_up((C * A + 255 * (255 - A)) / 255)
```

White means encoded RGB `(255,255,255)` in the retained source color space. It is
currently fixed, with no new configuration option or nonstandard IIIF parameter.
The operation consumes the raster and compacts storage in place; RGB and gray input pass
through. Source ICC remains unchanged. Background/color management can evolve
separately; this is not an implicit conversion to sRGB.

The 256 MiB TIFF decode budget counts the RGBA output plus one four-channel tile or full-width strip.
Resize counts input + output pixels + ICC; composition counts input pixels + ICC.
Vector capacity, codec allocations and caller-owned data are not a process-wide
cap. PNG and JPEG retain the 64 MiB HTTP response cap. Generic errors, authorization,
source version consistency, no-store and HEAD behavior remain unchanged.

After building/running tests, serve `build/Debug/http-test-data` and try:

```text
/alpha.tif/full/max/0/default.png
/alpha.tif/full/max/0/default.jpg
/alpha-associated.tif/3,1,29,17/14,8/0/default.png
/alpha-pyramid.tif/3,1,29,17/14,8/0/default.png
```

The pyramid fixture intentionally has a broken base tile to prove that a reduced
request uses its SubIFD; a full-resolution request fails intentionally.

## Verification and references

Alpha tests cover straight/associated decoding, missing/unspecified declarations,
corrupt associated samples, four-channel memory limits, ownership, in-place
composition, zero/partial coverage, hidden colors and an independent common-grid
filter oracle. HTTP tests use exact rational reference integration, independent
PNG decoding, lossy JPEG pixel comparison and HEAD. MinIO compares both formats
against local delivery of the same RGBA pyramid.

- [LibTIFF alpha representations](https://libtiff.gitlab.io/libtiff/functions/TIFFRGBAImage.html)
- [PNG specification](https://www.w3.org/TR/png-3/)
