# PNG input

PNG sources support information JSON and JPEG/PNG/WebP image delivery, locally and via
S3. The reader fully decodes a `RasterImage8`; existing window resampling performs
crop/reduction, followed by mirroring/rotation and encoding (see [arbitrary angles](arbitrary-rotation.md)). Even small crops
require full decoding. There is no PNG region decoding optimization.

## Supported pixels and color policy

- 8-bit RGB/RGBA and grayscale; grayscale with alpha stays native gray-alpha.
- Legal 1/2/4/8-bit grayscale and palettes expand losslessly to gray8 or RGB8.
- tRNS becomes alpha; gray stays gray-alpha, palette/RGB becomes RGBA8.
- Noninterlaced and Adam7 input. All seven passes combine into one raster.
- The decoder preserves ICC profiles without a color transform. When ICC is present it
  supplies the color interpretation; redundant gAMA/cHRM declarations are not copied.
- Explicit sRGB is preserved as a format-neutral declaration, including intent.
  PNG output retains the sRGB chunk; JPEG receives a matching RGB/gray ICC profile.
  This is profile assignment, not conversion. See [color management](color-management.md).
- The decoder preserves untagged samples. For a requested RGB-to-gray conversion,
  the shared quality stage now assumes sRGB; see the color policy above.

Explicit current exclusions:

- 16-bit, EXIF-bearing PNG (including Top-left), APNG, and CICP/HDR declarations.
- gAMA/cHRM without ICC or explicit sRGB: information remains readable, but pixel
  delivery is 501. These declarations are not silently discarded or assumed to be sRGB.


Normalize excluded sources on ingestion. Supporting additional encodings later
must be an explicit color/orientation/animation decision. Text and unrelated
ancillary metadata are not copied to derivatives. Unknown critical chunks fail.

## Reader, ownership and limits

`src/imgformats/PngReader.h/.cpp` uses pinned libpng through `ByteSource`. Each call
owns codec state, cursor, 16 KiB input buffer and returned data. There is no pathname
reopen, complete compressed-object staging or shared codec state.

`read_png_metadata` stops at the first IDAT header, retaining dimensions, original
color/depth, transparency/interlace, the presence of traditional color declarations
and ICC bytes or explicit sRGB intent. Palette/tRNS values are re-read by the pixel decoder from the same
version-bound source. Header success does not validate IDAT payload/CRC, IEND or
post-IDAT metadata; it is not proof that the image can be decoded.

`read_png_image` rechecks metadata, budgets the expanded raster and ICC, and applies
only lossless sample/layout expansion. It checks output channels/depth/row size,
reads every row/pass, then requires `png_read_end` through IEND. EXIF or animation
after IDAT is rejected too. Truncation/CRC/warning failures anywhere in this decode,
including outside a requested crop, never return partial pixels. No trailing-data
validation beyond IEND is promised.

Defaults: 16 MiB cumulative header input, 256 MiB total pixel-reader input, and
256 MiB for expanded raster plus ICC. Dimensions are limited to 1,000,000 per axis;
libpng's per-chunk allocation/decompression limit is 16 MiB, chunk cache count 128.
Unused text/suggested palettes are skipped without inflation. Native row/inflate
buffers, codec bookkeeping, saved markers, metadata copies and allocator overhead
are additional. These are per-call limits, not a process-wide RSS budget.
Downstream processing retains its own existing working limits.

Warnings and CRC failures are fatal. `PngReadError` covers malformed/truncated data,
stale metadata and limits; `PngUnsupportedError` covers unsupported encodings.
Storage exceptions propagate unchanged. Mutable codec state stays on the heap;
narrow setjmp boundaries never skip destruction of nontrivial C++ local objects.

## HTTP and caches

Signature detection is independent of filename. Preflight/source mapping and
version checks precede metadata-cache access. `PngMetadata` is part of the immutable
shared variant; ICC capacity is charged to its existing budget.

`supports_png_pixels` gates both info capabilities and image requests before result
lookup. Supported sources advertise mirroring/rotationBy90s and native opaque gray
also advertises gray quality. The user-approved missing `profile` remains unchanged.
The result identity includes the actual gray/RGB/RGBA layout. Only fully decoded,
processed and encoded results are published; failures add no entry. Authorization
and source-version checks still precede cache hits, including after restart.

Unsupported cases return 501; malformed source data returns 500 invalid_image_source;
missing sources and denials retain 404/403. No new configuration/dependency is needed.
Rendering recipe v2 now covers native gray-alpha preservation and the color-managed
`gray` quality; old encoded entries do not match. See [color management](color-management.md).

## Tests

`png-metadata` independently constructs filtered PNG data, including seven Adam7
passes, and compares decoded bytes with uncompressed sample references. Coverage
includes low-bit gray/palettes, alpha/tRNS, native gray/RGB ICC, strict truncation/CRC/
IEND validation, late EXIF/APNG, unsupported color declarations, limits, storage
errors, stale metadata, concurrent calls and cache accounting.

`png-http` checks exact output samples/ICC, crop/reduction/transforms, JPEG/PNG/HEAD,
prefixes/aliases, concurrent requests, authorization, cache hits/restart, no failed
publication, replacement and deletion. `s3-http` compares local/MinIO output bytes,
including alpha and interlaced sources.
