# Image sources and HTTP geometry validation

## Configuration and mapping

`--iiif-root`, `SIPI_IIIF_ROOT`, and TOML `iiif-root` select the optional local
repository, with CLI > environment > TOML > absent precedence. The loader checks
nonempty/NUL-free syntax. Server initialization requires a configured root to be
an existing directory and canonicalizes it before starting the listener. Relative
root values use process cwd, not the configuration file's directory. No directory
is created automatically. Help/version bypass directory access.

| Source decision | Resolution |
| --- | --- |
| No script, or allow without source; local default | Decoded identifier relative to iiif-root |
| No script, or allow without source; S3 default | Decoded identifier as key in s3-bucket |
| Allow with `{type="s3", key="..."}` | Literal key in configured s3-bucket |
| Allow with relative source | Script path relative to iiif-root |
| Allow with absolute source | Explicit trusted-script override; root not required |
| Deny | No source access |
| Relative/default source without root | 500 source_not_configured |

The default mapping does not include IIIF prefix segments, append an extension, or
URL-decode a second time. For example, `/museum/subpart/sample.tif/info.json`
uses `<root>/sample.tif` or key `sample.tif` in the configured bucket. A decoded slash may name a subdirectory within the root;
it cannot establish an absolute path or escape the root. Project-specific mapping
belongs in Lua. **The IIIF URL prefix stays outside the default mapping;
project-specific translation belongs in the preflight script.** Authorization still precedes all request-specific file access.

**Changed behavior:** relative Lua source results previously used cwd; they now
require iiif-root and are relative to it. The path to the Lua script itself still
uses cwd. Absolute script source results keep working without a configured root.

## Filesystem boundary

`src/image/SourceResolver.*` is independent of HTTP, Lua, and image codecs. It
returns an owning `ImageSource` variant: `LocalSource{path}` or `S3Source{bucket,key}`,
or throws typed SourceError. Its local-path resolver returns canonical regular
files; S3 selection does not access the network.
Relative/default sources reject parent traversal, rooted paths, NULs, and
backslashes. Canonical path components must remain within the canonical root:
symlinks inside the repository are permitted; symlinks leading outside are rejected.
A sibling directory sharing a name prefix does not pass this check.

Explicit absolute script overrides may intentionally select files outside the
repository. Scripts are trusted operator code, not arbitrary uploaded programs.
Nonexistent/non-regular sources return 404; forbidden relative paths return 403;
other filesystem errors return 500. Responses do not reveal local paths.

Canonicalization and opening the source are separate operations, not an atomic
capability-based open. Repository directories and symlinks must not be mutated by
untrusted writers during requests. After opening, one `LocalFileSource` serves
probing, metadata and pixel reads. Atomic replacement or unlinking of its path
does not change that opened source; in-place writes remain prohibited. See
[byte sources](byte-source.md).

## TIFF metadata and geometry

`src/http/IiifWorker.*` owns the optional Lua policy, source resolver, and size
policy, and receives the complete parsed image/info request. Its bounded worker
performs source resolution, opens one local or S3 byte source, then performs a twelve-byte
format probe, TIFF/JPEG 2000 metadata reading, and
image-request geometry validation away from the HTTP I/O thread.

The probe recognizes little/big-endian classic TIFF, BigTIFF, JP2 and raw J2K from bytes, without
requiring a filename extension. Other/unrecognized formats return
501 source_format_not_supported. A recognized TIFF/JPEG 2000 source with unreadable/corrupt metadata
returns 500 invalid_image_source. Storage read failures return 500 source_error;
a source disappearing before open returns 404 image_not_found. Information requests do not check pixel integrity; TIFF image output validates the accessed tiles or strips.

For image requests, orientation-normalized width/height are passed to the existing
format-neutral resolve_geometry function. No TIFF-specific branching is added to
that resolver. The startup size-upscaling setting is applied. Outside/invalid
regions and invalid sizes return 400 invalid_iiif_geometry. Actual enlargement
while upscaling is disabled returns 501 upscaling_disabled. `^` on a reduction is
valid. `^max` without a finite configured size limit returns 400; deployment size
limit configuration remains open. Invalid internal dimensions/policies return 500.

Successful geometry is concrete before rotation and can be inspected with debug
logging. Supported tiled or strip-based gray/RGB/RGBA8 TIFF crops, optionally reduced by area averaging, produce JPEG or PNG; enlargement, rotation
and unsupported layouts return 501 not_implemented. See [JPEG delivery](jpeg-delivery.md). An info request returns the
orientation-normalized dimensions and public identity as JSON with status 200;
it has no region/size to validate. See [info.json](iiif-info.md). All worker responses use Cache-Control: no-store.

## Reproducible test images and manual use

CTest generates real uncompressed RGB gradient TIFFs with libtiff, without network
access. `sample.tif` is 320 x 200; `rotated.tif` stores 320 x 200 with orientation 6
(display size 200 x 320); `large.tif` is a 1000 x 800 BigTIFF. They are build artifacts
in `build/Debug/http-test-data`, regenerated by the `http-tiff-fixtures` CTest fixture.

From the repository root:

```sh
cmake --build --preset conan-debug
ctest --preset conan-debug --output-on-failure
./build/Debug/sipi-2 --iiif-root build/Debug/http-test-data --log-level debug
```

- `/project/sample.tif/full/160,/0/default.jpg`: 200 JPEG, 160 x 100.
- `/sample.tif/320,0,1,1/max/0/default.jpg`: outside image, 400.
- `/sample.tif/full/640,/0/default.jpg`: enlargement without caret, 400.
- `/sample.tif/full/^640,/0/default.jpg`: 501 upscaling_disabled; with
  `--size-upscaling true`, 501 not_implemented after successful geometry.
- `/tiled.tif/full/max/0/default.jpg`: 200 JPEG, 320 x 200.
- `/tiled.tif/50,30,100,80/max/0/default.jpg`: 200 JPEG crop, 100 x 80.
- `/sample.tif/info.json`: 200 information JSON, dimensions 320 x 200.
- `/rotated.tif/info.json`: 501 source_not_deliverable; the header can be read, but this orientation cannot be delivered.

Automated checks include source priority, root startup failures, real symlinks and
traversal, Lua-selected alternate dimensions, HEAD, clipping, orientation, BigTIFF,
unsupported/corrupt headers, and enabled/disabled upscaling.

S3 backend parameters and MinIO examples are documented in [configuration](command-line.md#repository-selection-local-or-s3). Source opening remains after preflight; one shared S3 client serves requests, while each request pins its own opened object version.

JPEG 2000 JP2/raw J2K also supplies information, geometry checks and supported
8-bit gray/RGB region/resolution pixel delivery through the same local/S3 source;
see [JPEG 2000 input](jpeg2000-input.md).
