# HTTP routes

Drogon 1.9.13 serves plain HTTP on IPv4 loopback (`127.0.0.1`) with one I/O
thread. The port comes from CLI/environment/TOML/default configuration (8080).
This is a local development listener; deployment bind addresses and Caddy setup
are later steps. No TLS listener or certificates are configured in SIPI-2.

## Behavior

| Request | Result |
| --- | --- |
| GET `/hello` or `/hello/` | HTML containing `Hello World` |
| GET `/hello/id` | HTML containing the reconstructed request URL |
| GET `/hello/path1/path2/id?a=1&b=2` | Same, including path and query |
| GET `/health` | JSON: `{"status":"ok","message":"I'm alive","version":"0.1.0"}` |
| GET `/id/full/max/0/default.jpg` | 200 JPEG for supported tiled or strip-based gray/RGB/RGBA8 TIFF; otherwise explicit error |
| GET `/project/id/info.json` | 200 JSON: authorized TIFF dimensions and public service identity |
| GET an invalid IIIF path, including `/` | 400 JSON with error, component, and message |
| HEAD any of the above | Same status/headers, no response body |
| POST/PUT/DELETE/PATCH/OPTIONS on these paths | 405 JSON with `Allow: GET, HEAD` |

The version in `/health` comes from `SIPI_VERSION`, defined by CMake; it is not
hardcoded in the handler. Health is a **liveness** check, not a check of storage,
codecs, or readiness to process images.

Exact `/health` wins over regex routes. The `/hello(?:/.*)?` regex is registered
before the IIIF `/.*` catch-all. Thus `/hello/id/full/max/0/default.jpg` remains
an HTML hello request. `/health/id/info.json` is an IIIF request with prefix
`health`; only the exact health route is reserved. `/health/` currently produces
400. Drogon matches registered routes case-insensitively, so `/HEALTH` also works;
IIIF parameter keywords retain the parser's case-sensitive semantics.

All current application endpoints accept only GET/HEAD. A pre-routing advice
rejects other methods before they can fall through to IIIF or trigger automatic
Drogon CORS handling. This policy must evolve when adding a write endpoint such
as `/upload`. Framework-level requests such as `OPTIONS *` are handled by Drogon
outside the application routing policy. No CORS policy is configured here.

The IIIF handler passes `getOriginalPath()` to the parser, preserving percent
escapes for exactly one decode. Query parameters do not affect parsing. Invalid
syntax returns, for example:

```json
{"error":"invalid_iiif_syntax","component":"size","message":"..."}
```

The actual message explains the rejected syntax. Valid image and `info.json`
requests proceed to optional preflight. Without a script, they use the configured iiif-root mapping. Supported tiled or strip-based gray/RGB/RGBA8 TIFF crops return JPEG or PNG after metadata
and geometry checks. Unsupported layouts/operations return `not_implemented`
with status 501; valid arbitrary rotation angles use bilinear interpolation;
corrupt pixel reads return 500. Horizontal mirroring and clockwise quarter turns
are applied after crop/size; see [orthogonal transforms](orthogonal-transforms.md). See [JPEG delivery](jpeg-delivery.md). Information requests return actual metadata; see
[info.json](iiif-info.md) for the explicitly incomplete compliance declaration.
Information requests validate headers, while successful JPEG output also requires pixel decoding.
There is no temporary success JSON representation of parsed requests. Debug logs
record the identifier and prefix count, plus rotation/mirroring/upscaling for image
requests (`--log-level debug`). See [the parser contract](iiif-parser.md).

Static file serving is disabled. URI text in hello responses is HTML-escaped
before insertion into a text node; JSON responses use Drogon's JSON serializer.

The displayed URL is reconstructed as `http://` + Host (including any supplied
port) + original encoded path + raw nonempty query. An absent Host falls back to
the local socket address. URL fragments are not sent by browsers and cannot be
shown. An empty trailing `?` is not preserved. Host is echoed as untrusted text,
not used as an authorization decision. Forwarded headers are intentionally not
used until a trusted-proxy policy exists; behind Caddy this is the backend URL,
not necessarily the public HTTPS URL.

## Code and lifecycle

`main` loads configuration, handles help/version, sets the application's spdlog
level, validates an optional script, and transfers it to `run_http_server(config, policy)`. `HttpServer.cpp` registers free handler
functions, a method-checking advice, and an IIIF handler retaining the optional worker. Each handler
creates a response and invokes Drogon's callback; shared ownership keeps it alive for transmission.
IIIF processing uses a bounded worker pool in `src/http/IiifWorker.*`,
with at most 64 queued requests. Lua, filesystem checks, TIFF metadata, and geometry run there, not on the
I/O thread. Queued jobs own decoded request data and a completion retaining the
HTTP request. Drogon's response callback schedules transmission on its I/O loop.
After app.run returns, stop rejects new jobs, discards queued jobs, and joins all
active jobs; the worker owns the script snapshot throughout. Disconnected clients
may still incur evaluation; cancellation-aware delivery is deferred.

Drogon may create its default `uploads/` runtime directory in the working
folder. It is ignored by Git; this route does not process uploaded files.

## Build and validation

Use the macOS Conan profile and commands in README. The Conan recipe requires
OpenSSL even for this HTTP-only application; no custom dependency recipe is
introduced just to remove it. Trantor's Apple Clang 21 warning exception is
explicitly scoped in `profiles/macos-debug`.

`ctest --preset conan-debug --output-on-failure` runs configuration tests and a
real-process HTTP test using Python's standard library. The HTTP test checks
HTML, health/version JSON, URL/path/query preservation, escaping, route priority,
IIIF errors and valid requests, percent decoding, methods/Allow headers, HEAD, and
SIGTERM termination. Pure parser and configuration tests run separately. It uses a temporary working directory and a free port;
there is a small bind race between selecting that port and server startup.

Caddy forwarding, image output/cache streaming, load behavior, and Linux builds
remain untested and are separate increments.

## IIIF preflight and source resolution

For both image and info requests, including HEAD: parse syntax, run the configured
preflight if present, resolve and open one byte source, read TIFF metadata, and validate geometry.
Format probing and pixel decoding reuse that source (see [byte sources](byte-source.md)). Health/hello and rejected methods
never enter preflight. Authorization is evaluated on each request, including encoded-result cache hits.
The Authorization header is copied without validation by the HTTP layer. Drogon
parses Cookie separately and does not retain its raw header: use request.cookies
in Lua, e.g. request.cookies.session. Repeated cookie names follow Drogon's map
semantics; raw header ordering/duplicates cannot be recovered by this adapter.

| Outcome | HTTP status / error |
| --- | --- |
| Explicit deny | 403 / access_denied |
| Lua/runtime/contract/quota failure | 500 / preflight_failed |
| No script / allow without source | Resolve identifier within iiif-root |
| Required iiif-root absent | 500 / source_not_configured |
| Forbidden relative path / escaping symlink | 403 / source_not_allowed |
| Missing source or non-regular file | 404 / image_not_found |
| Other filesystem status/open/read failure | 500 / source_error |
| Unsupported source format | 501 / source_format_not_supported |
| Invalid TIFF metadata | 500 / invalid_image_source |
| Invalid region/size for the image | 400 / invalid_iiif_geometry |
| Actual upscaling disabled | 501 / upscaling_disabled |
| TIFF metadata validated for info.json | 200 / information document |
| Supported TIFF crop and JPEG/PNG output | 200 / image/jpeg or image/png |
| Invalid rotation syntax/range | Existing parser error mapping (400) |
| Unsupported TIFF pixel layout or transformation/output format | 501 / not_implemented |
| Corrupt TIFF pixels / decoder limit failure | 500 / invalid_image_source |
| JPEG/PNG codec or encoded-response limit failure | 500 / encoding_failed |
| Worker queue full or stopping | 503 / iiif_busy, Retry-After: 1 |

All preflight-dependent responses carry Cache-Control: no-store. Lua diagnostics,
credentials, and local paths are not included in responses; logs record error
categories without script-controlled error messages. HEAD has identical status
and policy handling, with its body removed by Drogon.

Relative/default sources use iiif-root and must stay inside its canonical path.
Absolute script overrides are deliberately permitted outside that root. Default
mapping ignores prefix segments; there is no implicit cwd root. See
[image source resolution](source-resolution.md) for filesystem assumptions,
TIFF/geometry behavior, and reproducible manual tests.

Tests additionally exercise denied/failed GET and HEAD, source selection, cookies,
encoded identifiers, quota failures, responsiveness of health during queued work,
recovery after failures, and absence of sensitive diagnostics. A deterministic
worker test covers the 64-job bound and shutdown rejection.

## S3 delivery

`iiif-storage=s3` selects the configured S3 bucket; a local default can also use
Lua S3 overrides. The worker authorizes before opening one version-bound source,
then uses the existing TIFF/geometry/JPEG pipeline. `/museum/tiled.tif/...` reads
key `tiled.tif`: IIIF URL prefixes are excluded from default mapping. Explicit
project mapping belongs in preflight. See [configuration](command-line.md#repository-selection-local-or-s3)
and [Lua source selection](preflight.md).

JPEG delivery now supports area-weighted reductions after crop reading, on both local and S3 sources. See [area resizing](area-resize.md) for channel semantics and memory limits. Delivery selects a suitable supported TIFF pyramid level and resamples the exact fractional crop within its integer read rectangle.

RGB8 PNG shares the processing path; see [PNG delivery](png-delivery.md) for the encoder and profile/error contract.


## Encoded-result cache

With `cache-dir` configured, the worker shares ownership of the persistent store.
Preflight and definitive source/version opening, metadata, geometry and source/layout
capability checks precede lookup on every request. A hit reads complete encoded
bytes and skips decoding/resizing/encoding. A miss publishes only successful output.
Lookup/read failures render normally; publication/admission failures still deliver
the completed encoding. Both paths retain the 64 MiB response bound and identical
content type and `Cache-Control: no-store`. No cache identifiers are exposed in
headers. GET/HEAD share entries; Drogon suppresses HEAD bodies as before.

Information documents, errors and authorization decisions are not cached. Source
header facts can now come from the version-bound [memory cache](metadata-cache.md),
including when the persistent result cache is disabled. Independent requests run concurrently in the bounded pool; see
[worker concurrency](worker-pool.md).
See [persistent cache](persistent-cache.md#http-integration) for failure boundaries,
lifecycle, tests and deferred streaming work.


JPEG input also supports `info.json` and HEAD using the shared metadata cache.
Only 8-bit gray/RGB/YCbCr and absent/Top-left EXIF orientation are accepted. JPEG
pixels use full decode followed by the existing crop/resize/transform/encode and
result-cache pipeline; malformed source data returns 500.
See [JPEG input](jpeg-input.md) for the supported subset and limits.


PNG input supports information and pixel GET/HEAD through the existing metadata/
result caches, locally and over S3. Full decode expands palettes, low-bit gray and
transparency before common processing. Unsupported color encodings return 501;
malformed data returns 500. See [PNG input](png-input.md).

Arbitrary angles expand the canvas with transparent borders (white for JPEG),
using alpha-weighted bilinear interpolation. See [rotation](arbitrary-rotation.md).
