# RGB/RGBA8 PNG delivery

The existing TIFF pipeline now accepts `.png` as well as `.jpg`. It performs the
same authorization, source selection, geometry validation, pyramid/tile selection
and fractional-window reduction before choosing the encoder. Supported qualities
are `default` and `color`; source support remains top-left tiled RGB/RGBA8 TIFF/BigTIFF.
PNG output preserves straight RGBA transparency. It does not yet add PNG input, gray/bitonal output, enlargement,
mirroring or rotation. `info.json` conformance declarations remain unchanged.

After building and running the tests:

```sh
./build/Debug/sipi-2 --iiif-root build/Debug/http-test-data
```

Example: `/tiled.tif/50,30,100,80/37,29/0/default.png` returns a 37 x 29 PNG.
The same URI shape works with S3-backed sources and preflight overrides.

## Encoder contract

`src/imgformats/PngWriter.h/.cpp` provides `write_png(image, sink)` in the
`pngwriter` target. Conan pins libpng 1.6.58; CMake links `PNG::PNG` privately.
After pulling this change, run the normal Conan install command and reload CMake
in CLion. Existing locked dependencies retain their versions/revisions.

The encoder borrows the owning `RasterImage8` and writes noninterlaced 8-bit RGB or straight RGBA rows
with compression level 6. It emits borrowed blocks of at most 16 KiB to a
synchronous sink, without creating another raster or a complete PNG buffer.
Input samples are preserved losslessly; prior resampling remains a separate step.
There is no gamma/color conversion, implicit sRGB declaration or synthetic gAMA/
cHRM chunk. ICC bytes are compressed into iCCP and preserved unchanged.

Nonempty ICC data must have a basic RGB ICC header (minimum length, signature,
RGB color-space field) and pass libpng's write checks. Warnings and failures abort
encoding rather than silently losing the profile. This is not complete ICC
validation: valid color transforms and missing/invalid-profile import policy
remain future color-management work. Test ICC headers deliberately contain no
transform tags and must not be used as production profiles.

Input dimensions are positive and at most 1,000,000 per axis, with exactly w*h*channel_count()
bytes and at most 256 MiB of pixel data; ICC is limited to 16 MiB. libpng additionally
allocates profile, compression and row/filter buffers. These are stage bounds,
not a process-wide memory cap. `PngWriteError` reports invalid input or codec
failures; allocation and sink exceptions can propagate separately.

Like the JPEG writer, mutable codec state lives on the heap and RAII releases it.
C callbacks catch sink exceptions, finish unwinding, then longjmp to the encoder's
recovery point. No C++ exception crosses libpng and no nontrivial C++ destructor
is skipped. Independent calls have no shared mutable codec state.

## HTTP and verification

The worker buffers encoded bytes up to 64 MiB and publishes `200 image/png` only
after successful completion. It uses `Cache-Control: no-store`. Any PNG codec or
encoded-output limit failure returns generic `500 encoding_failed` JSON; partial
PNG bytes never become a successful response. HEAD performs the same work and
returns matching headers without a body. Actual socket streaming remains future
work even though the encoder already emits blocks.

`PngWriterTests.cpp` tests invalid rasters/profiles, callback bounds and exceptions
at initial, middle and final output, followed by reuse. `png_check.py` independently
checks PNG CRCs, reverses all PNG row filters and inflates pixels/ICC using Python's
standard library. It compares samples and profiles byte for byte, including
single-pixel and one-dimensional images. HTTP tests exercise exact cropped/reduced
pixels, pyramids, inherited ICC, malformed-profile errors, MIME type and HEAD.
MinIO tests compare PNG output from the same pyramid locally and remotely.

See [alpha delivery](alpha-delivery.md) for TIFF alpha normalization, filtering and JPEG composition.
