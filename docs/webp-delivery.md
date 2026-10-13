# WebP delivery

Append .webp to any supported IIIF image request, for example:

```text
/image.tif/full/1000,/0/default.webp
/image.jp2/100,100,400,300/max/!90/default.webp
/image.png/full/max/0/bitonal.webp
```

All admitted TIFF/JPEG 2000/JPEG/PNG sources, local and S3, use the same existing
crop/resize/rotation/quality stages. WebP input is not introduced. info.json lists
extraFormats: ["webp"]; no preferredFormats override is imposed on clients.
Responses use image/webp, retain alpha, include canonical/profile links and CORS,
and follow existing authorization, HEAD, output-limit and cache behavior.

## Encoding choices

libwebp 1.6.0 is pinned through Conan and conan.lock. The independent webpwriter
target exposes write_webp with an explicit lossless flag and quality integer.
The HTTP layer currently supplies these fixed choices:

| Result | Mode | Quality/effort | Method |
| --- | --- | --- | --- |
| Color default/color | Lossy | 85 | 4 |
| Native grayscale, gray quality, bitonal | Lossless | 85 | 4 |

In lossless mode quality controls compression effort rather than fidelity. Alpha
compression is lossless in both modes. exact=1 preserves hidden transparent color
in lossless encoding. Codec internal threading is off because SIPI's worker pool
already bounds concurrent render jobs. These WebP settings are fixed; there are no CLI/env/TOML WebP encoding options.
JPEG has separate [configurable quality/sampling](jpeg-delivery.md).
Lossless refers to prepared encoder pixels, not to undoing resize/color conversion.
Gray is stored as RGB, and bitonal WebP is not a one-bit PNG representation.

## Color handling

WebP carries RGB/RGBA samples, so a GRAY ICC profile cannot describe its output.
Color handling is explicit and tested:

| Input to writer | Output policy |
| --- | --- |
| RGB/RGBA with RGB ICC | Preserve ICC bytes; no profile conversion |
| RGB/RGBA with explicit sRGB | Attach matching embedded RGB sRGB profile |
| Untagged RGB/RGBA | Remain untagged; interpreted as sRGB by WebP consumers |
| Gray/gray-alpha with ICC | LittleCMS conversion from that GRAY profile to RGB sRGB |
| Gray/gray-alpha with explicit sRGB | Use the declared sRGB gray curve, convert to RGB sRGB |
| Untagged gray/gray-alpha | Assume Gray Gamma 2.2 D50 for this required conversion |

The gray transform uses relative colorimetric intent and black-point compensation;
alpha is copied unchanged. The output receives our deterministic embedded RGB sRGB
profile. Invalid/unusable gray profiles fail instead of being dropped or replaced
with a fallback. RGB profile checks are structural, as with PNG, not full semantic
validation. The source image and other output encoders are unaffected. EXIF/IPTC,
XMP and animation are not emitted. See [color management](color-management.md).

## Ownership, limits and errors

Each call owns its WebPPicture, color context/profiles/transform, encoded buffer and
mux output. C callbacks catch exceptions and rethrow after the encoder returns.
The input raster is borrowed and unchanged. Output goes to the synchronous sink
in blocks of at most 16 KiB after encoding and ICC muxing complete. This is buffered
output, not streaming: it can coexist with the HTTP response buffer and native
codec scratch. RAII releases all codec and color resources on failures.

The format limits each output axis to 16383 pixels. The HTTP worker checks actual
rotated dimensions before cache lookup and returns 400 unsupported_webp_dimensions
if exceeded. This format-specific bound does not lower JPEG/PNG limits. Smaller
WebP requests remain usable for wide sources. General maxWidth/maxHeight/maxArea
and existing processing budgets still apply.

The writer budgets input plus its ARGB raster and source ICC under 256 MiB, with
128 KiB reserved for its gray conversion row/generated profiles. ICC is limited
to 16 MiB; encoded and assembled buffers each have a 64 MiB ceiling, with a
conservative pre-mux size check. Native codec/mux/color scratch and allocator
capacity are additional; these are not a process-wide memory guarantee. Encoding
or color errors return 500 encoding_failed; no partial success is cached/published.

## Persistent cache

WebpEncoding explicitly carries lossless and quality settings. Keys also contain
a WebP-specific recipe naming codec version, method, exact mode and gray color
policy. JPEG/PNG keys remain unchanged. The SQLite media whitelist accepts
image/webp without a schema migration. Format, settings and quality cannot collide
with JPEG/PNG entries. Headers are rebuilt on hits, and authorization/limits still
run before lookup. Restart tests confirm WebP persistence and identical bytes.

## Build and verification

After updating the checkout, install dependencies and reload CMake in CLion:

```sh
conan install . -pr:h profiles/macos-debug -pr:b sipi-2-local --build=missing
cmake --preset conan-debug
cmake --build --preset conan-debug
ctest --preset conan-debug --output-on-failure
```

The existing lock was extended only with libwebp; other pinned recipes remain.
The shared embedded-profile implementation is now the colorprofiles target used
by JPEG and WebP; no JPEG pixel/profile behavior changed.

Writer tests decode output with libwebp and inspect ICC chunks through its demuxer:
lossless pixels including hidden colors, lossy alpha, gray tone curves, explicit
sRGB/GRAY profiles, gray-alpha, malformed input, sink errors and deterministic bytes.
HTTP tests inspect RIFF/chunks and decode real results, including bitonal black/white,
HEAD, canonical links, restart/cache, denial, format separation and dimension limits.
MinIO tests compare local/S3 WebP bytes from all four source families. A synthetic
gray TIFF with an intentionally header-only ICC correctly fails conversion on both
backends; it is retained as a negative test, not silently normalized.

All 52 Debug suites pass, plus focused sanitizer checks. The earlier consortium
validator report predates WebP output and is not evidence for this new format.
