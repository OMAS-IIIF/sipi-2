# JPEG input

JPEG sources support information JSON and JPEG/PNG image delivery, locally and
through S3. The reader fully decodes an owning `RasterImage8`; the existing window
resampler then crops and reduces it, followed by mirroring/rotation and encoding (see [arbitrary angles](arbitrary-rotation.md)).
There is no ROI decoding or scaled IDCT yet: even a small crop requires the full raster.

## Accepted headers

- 8-bit grayscale and three-component RGB or YCbCr (ordinary color JPEG).
- Baseline and progressive JPEG headers.
- EXIF orientation absent or 1 (Top-left).
- ICC data in APP2, assembled in sequence order even if markers occur out of order.

CMYK/YCCK, other precisions and EXIF orientations 2..8 are unsupported. Normalize
orientation at ingestion; the delivery path does not silently ignore orientation
or report swapped dimensions for these rejected JPEG sources. Invalid orientation
values/types/counts, duplicate orientation tags and malformed recognized EXIF data
are errors. XMP and other unrelated APP1 markers are ignored.

ICC chunks require consistent counts, unique in-range sequence numbers and a
complete sequence of nonempty chunks. The bytes are retained unchanged without
color conversion or semantic validation of the profile. Unknown APP2 payloads
are ignored. EXIF processing inspects only the TIFF IFD0 orientation, with explicit
byte-order/offset/entry bounds; it never follows thumbnail/Exif/GPS offsets or uses
EXIF pixel dimensions instead of JPEG SOF dimensions.

## Storage, ownership and resource limits

`src/imgformats/JpegReader.h/.cpp` provides `read_jpeg_metadata(const ByteSource&,
max_read_bytes)` in the `jpegreader` target, with the already-pinned libjpeg/9f.
`JpegMetadata` owns width/height, logical gray/RGB color, progressive flag and ICC
bytes. There are no borrowed codec pointers. Each call has an independent codec,
cursor, buffer and error boundary, permitting concurrent local/S3 reads.

The source is borrowed and version-bound, without a path reopen or whole-object
staging. The source manager reads in at most 16 KiB chunks. Default cumulative
input limit is 16 MiB; no compressed pixels are decoded. `jpeg_read_header` stops
at the first SOS, but buffering may prefetch some compressed data. Saved APP1/APP2
payload is bounded by input; ICC assembly can add another copy of at most 16 MiB.
Codec/marker bookkeeping and allocator overhead are additional. This is not an
RSS quota. Very large headers can be rejected even if the image is otherwise valid.

Heap-owned state and a C error boundary follow the existing JpegWriter pattern.
Codec warnings and errors are fatal, and EOF never synthesizes a successful EOI.
Storage exceptions are captured inside callbacks, fully unwound, then rethrown
unchanged outside the codec. `JpegReadError` denotes malformed/truncated headers or
budget failures; `JpegUnsupportedError` denotes the supported-subset boundary.

**Header success does not validate the complete JPEG.** Entropy data, EOI and any
metadata following the first SOS are not examined. Even a file truncated just
after SOS can provide metadata; pixel decoding validates entropy data and EOI separately. EXIF/ICC interpretation
remains limited to markers preceding the first SOS.

## HTTP and cache integration

The worker identifies JPEG by SOI/marker bytes, not filename extension. It performs
normal preflight, source mapping and version checks, then uses the same immutable
metadata LRU as TIFF/J2K. The variant includes JpegMetadata and accounts for its ICC
capacity. Both local files and S3 range reads use the same reader. Replacement or
deletion is checked before cache reuse; errors are not cached.

GET/HEAD `/{prefix...}/{identifier}/info.json` uses the existing format-neutral
information builder with accepted JPEG dimensions. Public identity remains the
requested URL, including its prefix, regardless of preflight source aliases.
Accepted sources advertise mirroring and rotationBy90s; native grayscale also
advertises gray quality. The owner-approved missing-`profile` development boundary
remains in place.

| Situation | Response |
| --- | --- |
| Accepted JPEG info request | 200, application/json, no-store |
| Valid image request from a JPEG source | 200, image/jpeg or image/png |
| Unsupported JPEG layout/EXIF orientation | 501, not_implemented |
| Malformed/truncated data, invalid ICC/EXIF, reader budget exceeded | 500, invalid_image_source |
| Invalid IIIF geometry | Existing 400/501 geometry mapping |
| Missing source / preflight denial | Existing 404 / 403 |

Image geometry is checked using common dimensions. The persistent result cache uses
the existing version-bound source, geometry, quality and encoding identity. Only
successfully encoded images are published; failed reads/encodes create no entry.
The rendering recipe is unchanged because existing output bytes are unaffected and
JPEG input previously had no cached results. JPEG decoding fixes JDCT_ISLOW and fancy
upsampling explicitly; future output-affecting changes must update that recipe.

`read_jpeg_image` rechecks header facts/ICC against the metadata snapshot, then decodes
all rows and requires successful completion through EOI. Warnings are fatal, including
truncation outside the requested crop. Defaults are 256 MiB cumulative compressed
input and 256 MiB for pixels, ICC and estimated retained coefficient arrays for
progressive/multiscan images. These checks precede pixel allocation and decompressor
startup. Header reads retain the 16 MiB cap. Native scratch, markers, metadata copies
and allocator overhead are additional; these are not process-wide memory quotas.
The resampler and later operations retain their own existing working limits.

ICC bytes survive the raster pipeline without color conversion. Writers can reject
semantically invalid or mismatched profiles (500 encoding_failed); header acceptance
alone does not guarantee encodability. EXIF and unrelated application markers are not
copied to derivatives.

## Tests

`jpeg-metadata` generates baseline, progressive, native gray, direct RGB and CMYK
headers; tests YCCK/12-bit rejection, EXIF in both byte orders, orientations 1..8,
malformed offsets/values/duplicates, out-of-order/incomplete/duplicate ICC chunks,
read budgets, storage exception identity, no full-source read, header-only input,
parallel calls and shared-cache ICC accounting. Pixel checks compare against an
independent libjpeg memory-source decode and cover baseline/progressive/gray/direct
RGB, ICC retention, stale metadata, missing EOI and memory/input limits. It also
creates HTTP fixtures.

`jpeg-http` exercises GET/HEAD, prefixes, extensionless IDs, preflight aliases and
denial, unsupported/corrupt sources, metadata reuse, crop pixel equality, reduction,
mirrored rotation, ICC preservation, result-cache reuse and same-ID replacement by
TIFF. A tiny crop of a truncated image fails without cache publication. MinIO tests
compare local/S3 JPEG and PNG output bytes, including cropped/reduced/rotated images.
