# First TIFF-to-JPEG delivery

The HTTP worker now delivers real image pixels after the existing syntax,
preflight, source, metadata and geometry checks. This is a deliberately narrow
codec increment, not a general image transformation engine.

## Supported requests

Source: tiled TIFF/BigTIFF, top-left orientation, unsigned 8-bit contiguous RGB,
exactly three samples, no extra samples/alpha, no volume depth. The existing TIFF
reader selects only tiles intersecting the crop, at full source resolution.
Strips, other orientations/sample layouts, and pyramid selection remain separate
steps. Metadata reading still supports more layouts than pixel delivery.

All existing region forms (full, square, pixel and percentage) work through the
geometry resolver, including clipping at image edges. The resolved output size
must equal the crop dimensions: no scaling is performed. Rotation 0 or 360 is
accepted as identity; mirroring and other rotations return 501. JPEG with default
or color quality is supported; gray/bitonal and other output formats return 501.
Invalid geometry retains its previous 400/501 error mapping before rendering.

After building and running CTest, from the repository root:

```sh
./build/Debug/sipi-2 --iiif-root build/Debug/http-test-data
```

- `/tiled.tif/full/max/0/default.jpg`: 320 x 200 JPEG.
- `/project/tiled.tif/50,30,100,80/max/0/default.jpg`: 100 x 80 crop.
- `/tiled.tif/square/max/0/default.jpg`: centered 200 x 200 square.
- `/tiled.tif/pct:25,25,50,50/max/0/default.jpg`: 160 x 100 crop.
- `/tiled.tif/full/160,/0/default.jpg`: 501, resizing not implemented.
- `/sample.tif/full/max/0/default.jpg`: 501, the older fixture uses strips.

Preflight can select the source as before. Information requests are unchanged;
profile publication remains pending a per-resource conformance review. This first
supported subset does not make every metadata-readable TIFF deliverable.

## Encoder boundary and ownership

`src/imgformats/JpegWriter.h` exposes `write_jpeg(image, sink, quality=90)`.
The encoder depends on the format-neutral owning `RgbImage8`, not TIFF, Drogon,
files or a cache. libjpeg 9f is now an explicit Conan dependency and is linked
privately by the `jpegwriter` CMake target; the locked version was already used
transitively by libtiff.

The sink synchronously consumes spans of at most 16 KiB. Spans borrow a codec
buffer and must not be retained. The raster and callback remain caller-owned and
live throughout encoding. A sink exception aborts output and propagates unchanged.
Any previously received bytes must be discarded or their destination aborted.
Successful return, not a final callback, signals complete encoding.

Baseline JPEG uses quality 90 and 4:4:4 sampling. The quality argument is 1..100;
HTTP currently uses the default. ICC bytes are preserved exactly in ordered APP2
chunks; no profile validation or color conversion occurs. Untagged RGB remains
untagged. RGB is not automatically sRGB. EXIF/IPTC and other TIFF metadata are not
copied; long-term metadata/original preservation remains a separate decision.

libjpeg uses C error callbacks. The encoder holds mutable codec/error state on the
heap, created before setjmp, and destroys codec allocations with RAII on every
exit. Fatal errors jump only across C/trivial frames. Throwing sinks unwind first;
the callback saves the exception, leaves its catch scope, then jumps. C++ exceptions
never cross the C library and longjmp never skips a live nontrivial C++ destructor.

## HTTP buffering and limits

The worker accumulates output in a string limited to 64 MiB, then moves it into
Drogon's response. It sends 200 `image/jpeg` and `Cache-Control: no-store` only
when decoding and encoding have completed. Partial output cannot become a
successful response. The same encoder interface can later feed a socket/cache
sink, but actual backpressure, cancellation and cache commit are not implemented.

The TIFF reader's decoded crop plus tile buffer limit remains 256 MiB, with its
existing separate metadata/codec allocation bounds. libjpeg supports dimensions
up to 65500 in this build. ICC must fit 255 chunks of 65519 bytes. These are bounds
on individual processing stages, not a promise of a total process memory ceiling.

Unsupported TIFF layout raises TiffUnsupportedError (a TiffReadError subtype),
mapped to 501 `not_implemented`. Detected corruption and decoded-byte limit errors
map to 500 `invalid_image_source`. Encoder failures and encoded-byte limit errors
map to 500 `encoding_failed`. No codec diagnostics or local paths reach clients.
Repository files must remain stable between metadata planning and reopening for
pixel reads, as already required by source resolution.

HEAD currently performs the same decoding/encoding as GET, so status and content
length match, then Drogon suppresses its body. A future optimization needs to keep
this validation contract explicit.

## Verification

`jpeg-writer` checks decoded gradient pixels, dimensions, multi-block output,
ICC chunk ordering/byte preservation, invalid inputs and sink exceptions during
full-block/final flush, followed by successful reuse of the API.

HTTP tests decode complete JPEG responses using the test executable and compare
all crop pixels (with tolerance for lossy encoding). Cases include tile crossings,
edge clipping, square/percentage regions, identity size/rotation, source overrides,
GET/HEAD parity, unsupported operations, corrupt tiles inside/outside the crop,
and successful delivery after errors. TIFF fixtures are generated locally with
libtiff; no external image or Python imaging package is required.
