# Feature catalog

This catalog describes the implemented SIPI-2 **0.1.3** feature set, reviewed on
2026-10-13 against the source, configuration defaults and regression contracts.
It distinguishes supported behavior from restrictions and configuration-dependent
features. It is not a roadmap or a claim that every possible source encoding has
been tested. Detailed contracts are linked beside each topic.

SIPI-2 implements the **IIIF Image API 3** for images in local or S3-compatible
repositories. The pipeline uses an 8-bit gray/gray-alpha/RGB/RGBA raster model;
each input reader supports a specific subset of those layouts. The application
is C++20 with a Drogon HTTP frontend, Lua policies and dedicated image workers.

## Contents

- [Input formats](#input-formats)
- [Output formats](#output-formats)
- [IIIF requests and discovery](#iiif-requests-and-discovery)
- [Image processing and color](#image-processing-and-color)
- [Storage and source identity](#storage-and-source-identity)
- [Access policies and restricted images](#access-policies-and-restricted-images)
- [Watermarks](#watermarks)
- [Lua integration](#lua-integration)
- [Caches and concurrency](#caches-and-concurrency)
- [Configuration and operational limits](#configuration-and-operational-limits)
- [HTTP and deployment](#http-and-deployment)
- [Development and validation](#development-and-validation)
- [Explicit exclusions](#explicit-exclusions)

## Input formats

All supported input families work through the local/S3 byte-source boundary and
can feed any of the three output encoders. Formats are detected from content;
a filename extension alone does not establish decode support. Source admission
checks the supported layout and predictable resource requirements before serving
`info.json` or an image. Readable metadata does not prove that pixels can be decoded
or that the entire file is valid.

| Input | Supported delivery | Important restrictions |
| --- | --- | --- |
| Classic TIFF / BigTIFF | Tiled or strip-based, pyramidal or single-resolution; unsigned 8-bit contiguous RGB, explicitly tagged RGBA, MINISBLACK/MINISWHITE grayscale | Top-left orientation only; no separate planes, gray-alpha TIFF, palette, CMYK/Lab, 1-bit, 16-bit or floating-point delivery |
| JPEG 2000 (JP2 / raw J2K) | Unsigned 8-bit equally sampled grayscale or RGB; region/resolution decoding through Grok | Color must be explicitly supported or have a matching ICC declaration; untagged raw multicomponent codestreams are rejected; no alpha, palette/channel mappings, YCbCr/CMYK, signed samples or other precisions |
| HTJ2K (JPH / raw HT codestream) | Same gray/RGB pixel contract and delivery pipeline as classic JPEG 2000 | Part-15 input only; raw multicomponent color remains ambiguous; does not add higher precision, alpha or new response formats |
| JPEG | Baseline and progressive; 8-bit grayscale and ordinary RGB/YCbCr color; ICC extraction | EXIF orientation absent or Top-left only; no CMYK/YCCK or other sample precisions; full image decode even for a small crop |
| PNG | Gray, gray-alpha, RGB, RGBA; legal low-bit grayscale/palette expansion; tRNS; noninterlaced and Adam7 | No 16-bit, EXIF-bearing PNG, APNG or CICP/HDR. gAMA/cHRM without ICC or explicit sRGB is not supported for pixel delivery; full image decode even for a small crop |

TIFF selects a suitable compatible pyramid level and reads intersecting tiles or
strips. A strip spans the full image width, so a one-strip image may require a full
strip decode for a tiny crop. Tested TIFF compression cases include uncompressed,
LZW, Deflate and PackBits; other compression requires a suitable libtiff codec and
is not implied by recognizing TIFF. JPEG 2000 selects a resolution and region
through Grok; reduced-level output need not equal full-resolution resampling.
Raw single-component JPEG 2000 uses the repository's grayscale convention.

References: [TIFF planning](tiff-metadata-and-geometry.md),
[strips/grayscale](tiff-strips-and-gray.md), [TIFF alpha](alpha-delivery.md),
[JPEG 2000 and HTJ2K](jpeg2000-input.md), [JPEG](jpeg-input.md), [PNG](png-input.md).

## Output formats

| IIIF extension | Encoding and color | Alpha and settings |
| --- | --- | --- |
| `.jpg` | JPEG gray or RGB; ICC retained or assigned according to the color contract | Alpha composited on white. Server-wide quality 1–100, default 90; sampling 4:4:4 (default) or 4:2:0; gray ignores chroma sampling |
| `.png` | Lossless encoding of processed samples; gray/gray-alpha/RGB/RGBA; bitonal encoded as packed 1-bit gray | Alpha retained except after bitonal compositing; ICC or explicit sRGB declarations preserved as appropriate |
| `.webp` | Color default/color output is lossy, quality 85; native gray, gray and bitonal output use lossless encoding | Alpha retained and encoded losslessly. Fixed encoder settings; no CLI quality/sampling option for WebP. Gray is converted to an RGB representation appropriate for WebP |

Lossless output encoding does not undo losses introduced by source compression,
resizing, conversion or thresholding. JPEG bitonal output can contain values other
than pure black/white because JPEG compression is lossy. Output metadata handling
is color-focused; arbitrary source EXIF/XMP/text metadata is not copied wholesale.
No TIFF, JPEG 2000, GIF or PDF encoder is exposed, even where the parser recognizes
an IIIF format name. Unsupported output formats return HTTP 400.

References: [JPEG encoding](jpeg-delivery.md), [PNG encoding](png-delivery.md),
[WebP encoding and color rules](webp-delivery.md).

## IIIF requests and discovery

The image request form is
`/{prefix...}/{identifier}/{region}/{size}/{rotation}/{quality}.{format}`.
An optional prefix is part of the public identity, not an implicit storage path.
URL path segments are decoded once; a literal `+` stays `+` in the path.

| Component | Supported forms / behavior |
| --- | --- |
| Region | `full`, centered `square`, pixel `x,y,w,h`, percentage `pct:x,y,w,h`; geometry validation and clipping against the effective image |
| Size | `max`, `w,`, `,h`, exact `w,h`, best fit `!w,h`, percentage `pct:n` |
| Enlargement | Leading `^` permits enlargement only when the operator also enables `size-upscaling`; output limits still apply |
| Rotation | Clockwise angles from 0 through 360; optional `!` horizontal mirroring before rotation |
| Quality | `default`, `color`, `gray`, `bitonal`; requesting color on a native grayscale source does not synthesize color |
| Information | `/{identifier}/info.json`, with optional prefix; public dimensions, service ID, `ImageService3`, `level2`, output bounds and applicable extras |
| Base URI | GET/HEAD authorizes the resource, then redirects with 303 to `info.json` |
| Advertised extras | Canonical/profile Link headers, mirroring, arbitrary rotation, gray/bitonal, color for color sources, WebP output; sizeUpscaling only when enabled |

`info.json` supports JSON and JSON-LD content negotiation. It currently emits no
`sizes`, `tiles` or `preferredFormats` entries: clients may request valid regions
and sizes, but should not expect a tile grid advertised there. Storage pyramids
are an internal optimization, not a public tiling contract. The configured public
base URL determines service IDs and redirects; source paths remain private.

The recorded external validation passes 78 checks across original and restricted
services. This is not formal certification or a guarantee for every viewer/source.
References: [parser](iiif-parser.md), [information](iiif-info.md),
[HTTP contract](iiif-http.md), [canonical URLs](canonical-uri.md),
[external validation](external-validation.md).

## Image processing and color

| Function | Behavior / boundary |
| --- | --- |
| Reduction | Area-weighted resampling with exact fractional windows; reusable per-axis weights; alpha-aware sampling |
| Enlargement | Bilinear interpolation, opt-in; constrained by both IIIF syntax and server policy |
| Mirror / quarter turns | Pixel-preserving rearrangement at the transform stage; other stages may still alter pixels |
| Arbitrary rotation | Bilinear sampling onto a bounding canvas with transparent uncovered areas; JPEG composites onto white |
| ICC and sRGB | Preserve supported source encoding/declarations for ordinary delivery; no unconditional conversion of all images to sRGB |
| RGB to gray | LittleCMS conversion to Gray Gamma 2.2/D50 with a corresponding ICC profile; usable RGB ICC or explicit sRGB is honored; untagged RGB assumes sRGB for this conversion |
| Native gray | Preserve samples and supported source profile/declaration; do not assign a new gamma simply because `gray` was requested |
| Bitonal | Convert to gray as needed, composite alpha on white, then threshold at 128; optional serpentine Floyd–Steinberg dithering |
| Transparency | Normalize supported associated TIFF alpha to straight alpha; filtering weights colors by coverage; PNG/WebP preserve supported output alpha |

Sampling and compositing operate on stored sample values, not a general linear-light
working space. An invalid profile required for a color conversion causes failure;
it does not silently become sRGB. JPEG's white background is fixed, not a request
parameter. Bitonal output is opaque. Dithering starts independently per requested
output raster, so independently rendered adjacent tiles may show different patterns
or seams. Threshold mode is the default.

References: [resampling](area-resize.md), [enlargement](upscaling.md),
[orthogonal transforms](orthogonal-transforms.md), [rotation](arbitrary-rotation.md),
[color management and bitonal](color-management.md), [alpha](alpha-delivery.md).

## Storage and source identity

| Capability | Behavior / boundary |
| --- | --- |
| Local repository | Identifier resolves relative to `iiif-root`; default mapping does not append extensions or include prefix segments |
| Local overrides | Lua may select a relative path within the root or an explicit absolute path; absolute paths are trusted operator overrides |
| Repository containment | Relative/default paths reject traversal and symlinks escaping the root; repository directories must not be mutated by untrusted writers during requests |
| S3-compatible repository | Configurable bucket, endpoint, region and path-style addressing; default credential provider chain or explicit credentials with optional session token |
| S3 source selection | Lua may select an object key in the configured bucket; it cannot supply per-request buckets, endpoints or credentials |
| Consistent reads | Source identity/version/length bind metadata, pixel reads and cache entries; S3 uses HEAD and version/ETag-bound range reads |
| Mixed policy selection | A Lua local path stays local even with an S3 default; an explicit S3 key selects S3 even with a local default |

S3 range reads are reads from the repository, not a promise of HTTP byte-range
responses to image clients. The server does not automatically upload, normalize,
convert or build pyramids for repository files. The HTTP helper in Lua is for
policy integration; it is not a generic HTTP-URL image-source adapter.
References: [source resolution](source-resolution.md), [byte sources](byte-source.md),
[S3](s3-byte-source.md).

## Access policies and restricted images

Lua `preflight(request)` runs for IIIF image, information and base-URI GET/HEAD
requests, before request-specific source access and before encoded-cache hits.
No preflight script is enabled by default; configured source access is then
unrestricted by Lua. Health, hello and OPTIONS do not execute image preflight.

| Capability | Behavior / boundary |
| --- | --- |
| Allow / deny | Exactly one validated result table; explicit denial is HTTP 403; invalid policies or runtime failures do not grant access |
| Source mapping | Select a local path or S3 key, independently of the public identifier |
| Application authorization | Script interprets Authorization, parsed cookies and query values; authentication scheme and resource binding belong to the application |
| Restricted public resolution | `resolution = {max_width=..., max_height=...}` defines a reduced whole-image canvas; all public regions/sizes and `info.json` use that canvas |
| Stable public identities | Use separate original/public service URLs; do not change dimensions at one URL according to login state |
| Required watermark | Combines with source mapping and resolution restriction; failed required mask validation never falls back to unmarked output |

**Output limits and restricted resolution are different.** A limit on each response
size does not prevent assembling original detail from many crops. Restricted
resolution instead derives all crops from the same reduced public raster. Its
cache is before watermark application. The server cannot infer aliases or enforce
application-wide access rules that the policy does not specify.
No built-in login/session service, IIIF Authorization Flow or automatic public/original
variant discovery is supplied. References: [preflight](preflight.md),
[restricted resolution](restricted-resolution.md), [OLDAP example](oldap-preflight.md).

## Watermarks

A policy can require one centered, single-color watermark anchored to the complete
effective image, so crops/tiles do not independently reposition it. Width is a
fraction of image width, fitted to height when needed; opacity and an sRGB color
are supplied separately. The watermark color is converted into the image encoding.

Masks are local or S3 **untagged grayscale PNGs**, 1/2/4/8-bit, optionally Adam7.
Black means transparent coverage and white means opaque. RGB/RGBA, gray-alpha,
palette, tRNS, color profiles/gamma declarations, 16-bit, EXIF and animated masks
are rejected. There is no automatic channel extraction or mask inversion.

Mask identity/version is checked before information responses and result-cache
lookup, including HEAD. The mark is applied before the requested crop/resize,
mirror/rotation and quality stages; resolution-restricted images use the public
canvas. Masks are bounded and can be cached in memory. Multicolor logos, repeated
patterns and freely positioned multiple marks are not implemented.
Reference: [watermark contract and processing](watermarks.md).

## Lua integration

| Facility | Supported interface / boundary |
| --- | --- |
| Request snapshot | Decoded prefix array and identifier, Authorization and optional raw Cookie field, parsed cookies, repeated query values as arrays, route kind and GET/HEAD method; no live HTTP connection or full arbitrary-header map |
| Policy lifetime | Script/configuration snapshotted and validated at startup; each evaluation has a fresh Lua state; globals are not persistent storage |
| Explicit settings | `sipi.config` and `sipi.secrets`; per-name CLI/environment/TOML precedence and startup secret-file snapshots |
| JSON | `sipi.json` encoding/decoding, explicit array/object identity and JSON null; strict conversion of supported values |
| HTTP(S) | `sipi.http.request`: synchronous bounded requests with explicit headers/body, TLS verification, status/headers/trailers/body; no automatic redirects, retries, credential forwarding or cookie jar |
| JWT | `sipi.jwt.sign` / `verify`: HS256 and RS256, explicit keys/algorithm and claim policy; no implicit permission grant, JWKS discovery or revocation service |
| Shared value cache | `sipi.cache.get`, `get_or_load`, revision-conditional `invalidate`; bounded per-policy TTL values and coordinated concurrent loads |
| Standard libraries | Restricted base facilities plus table/string/math/utf8; no io/os/package/debug, dynamic/file loading or console output |
| Execution budgets | 16 MiB Lua allocations and approximately one million VM instructions including initialization; native helper calls have their own bounds |

Policies are trusted operator programs, not a sandbox for arbitrary users. VM
instruction limits do not preempt native operations. Query parsing is bounded and
validated before policy execution; tokens are not automatically verified or included
in public service IDs. Query credentials must be reattached by clients to subsequent
image requests. Policy diagnostics are bounded/escaped and mask known configured
secrets and incoming credentials; scripts must avoid exposing transformed secrets.

References: [preflight contract](preflight.md), [settings/secrets](lua-configuration.md),
[JSON](lua-json.md), [HTTP](lua-http.md), [JWT](lua-jwt.md), [value cache](lua-cache.md).

## Caches and concurrency

| Cache | Persistence and default capacity | Behavior |
| --- | --- | --- |
| Encoded results | Disabled until `cache-dir` is configured; 1 GiB encoded-file budget, cleanup target 90% | File objects plus SQLite index, source/render-policy identity, LRU eviction and startup recovery; concurrent identical fills coordinated |
| Source metadata | Memory only; 256 entries / 64 MiB | Immutable source metadata shared across workers; current source version is checked before reuse |
| Watermark masks | Memory only; 64 entries / 32 MiB | Decoded coverage masks; current source identity/version checked before reuse |
| Reduced public images | Memory only; 16 entries / 64 MiB | Complete reduced rasters before watermark/IIIF transforms; disabling retention does not disable the restriction |
| Lua values | Memory only per policy; 256 entries / 4 MiB serialized keys/values | Explicit script-managed values, up to 64 KiB each; TTL 0–3600 seconds; restart/new policy empties the cache |

Either zero entry or byte limit disables retention in each of the three image-related
memory caches. Their limits are configurable; Lua cache bounds are implementation
limits. Cache sizes are accounting budgets, not total process memory quotas.
The result cache requires a private local directory and locks it to one instance;
it is not a distributed shared cache. Runtime cache storage failures can fall back
to uncached delivery. Cache hits do not bypass policy or required-mask validation.

A bounded worker pool handles policy, storage and rendering separately from HTTP I/O.
Default workers are detected logical CPUs clamped to 1–4; configurable range is
1–32. The queue allows 64 waiting jobs; overload returns HTTP 503 with
`Retry-After: 1`. Capacity is not automatically scaled to container memory. Shutdown
rejects new work, discards queued jobs and joins active workers.
References: [result cache](persistent-cache.md), [cache keys](cache-key.md),
[metadata](metadata-cache.md), [mask cache](watermarks.md#decoded-mask-memory-cache),
[public raster cache](restricted-resolution.md), [workers](worker-pool.md).

## Configuration and operational limits

Settings use **CLI > environment > explicitly selected TOML > default**. No file
is discovered implicitly. Settings and Lua files are adopted at startup; restart
to change them. CLI help/version are available without starting the HTTP service.

| Setting / bound | Default and configurability |
| --- | --- |
| Listener | Native `127.0.0.1:8080`; numeric IPv4/IPv6 address and port configurable; container defaults to `0.0.0.0:8080` |
| Public base URL | Defaults to loopback plus configured port; explicit HTTP(S) origin and optional mount path for proxies; request headers do not override it |
| Output width / height | Width 8192; absent height inherits width; positive configurable values, each at most 32768 |
| Output area | 8,388,608 pixels; positive configurable value up to that same ceiling |
| Enlargement / bitonal | Enlargement off; bitonal threshold by default, optional Floyd–Steinberg |
| JPEG | Quality 90 and 4:4:4 sampling by default; configurable as described above |
| Worker / caches | Configurable worker count and image-cache capacities as above |
| Raster work / response | Typically 256 MiB per processing-operation budget and 64 MiB buffered encoded response; implementation limits, not global RSS quotas |
| Watermark input / decoded mask | 16 MiB each; implementation limits |
| Lua / network helpers | Separate VM, request, timeout and response bounds; see helper contracts |

Public output bounds apply to the requested size before rotation; an expanded
rotation canvas has separate processing-budget checks. `full/max` fits output
policy, whereas explicit oversized sizes fail rather than being silently reduced.
Dimensions, byte budgets and concurrency all matter for large sources. Worker count,
codec internals, cache overhead and responses awaiting transmission can multiply
memory use beyond any individual processing budget.
Reference: [complete configuration reference](command-line.md),
[example TOML](../sipi.example.toml), [output limits](output-limits.md).

## HTTP and deployment

| Capability | Behavior / boundary |
| --- | --- |
| Methods | IIIF GET/HEAD and CORS OPTIONS; unsupported methods return 405; HEAD follows policy/status/header behavior without returning a body |
| CORS | Wildcard without credentials by default; explicit origin allowlist and credential mode configurable; wildcard plus credentials rejected |
| Discovery headers | Profile Link on image/info success; canonical Link on image responses; public identity preserved on cache hits |
| Negotiation | JSON/JSON-LD information response, `Vary: Accept`, 406 when no acceptable representation exists |
| Client caching | IIIF worker responses use `Cache-Control: no-store`; internal server result caching is independent |
| Errors | Structured HTTP errors; no source paths/credentials in response bodies; policy failure, missing source, unsupported layout and overload remain distinct |
| Health / diagnostics | `/health` reports liveness/version outside the image queue; application log levels and startup/runtime diagnostics; health does not validate every source or policy backend |
| Transport | Plain HTTP listener; HTTPS termination and external routing belong to a reverse proxy |
| Native | CMake/Conan build and direct executable; no container needed for delivery |
| Container | Published Linux AMD64/ARM64 image; UID/GID 10001; runtime image contains binary/libraries; mount media, policies/configuration and persistent cache as needed |

CORS is browser policy, not authentication. No built-in image-management/admin API,
Prometheus endpoint, hot reload, streaming image encoder response or distributed
cache service is provided. Diagnostic hello routes also exist but are not image
management endpoints. Generic container usage is independent of the optional
OLDAP example. References: [HTTP contract](iiif-http.md),
[server integration](http-server.md), [Docker](docker.md), [native build](building.md).

## Development and validation

CMake/Conan maintain the build and locked dependency graph, including the local
Grok recipe. Native CTests cover configuration, parsers, codecs, image operations,
Lua, source handling, caches and HTTP; optional MinIO tests exercise S3. The
external IIIF validator, Docker runtime checks and comparative benchmarks are
separate workflows. Sanitizer and benchmark reports document their specific scope.
A listed capability is not a claim that all these suites were rerun for this catalog.

The Makefile provides native builds, verification, version changes, local Docker
checks and explicit image publication. `make docker-build` publishes; it is not
just a local build. The public quickstart uses a synthetic image, independent of
private archival benchmark originals. References: [build](building.md),
[Makefile](makefile.md), [validation](external-validation.md),
[performance evidence](README.md#validation-and-performance).

## Explicit exclusions

The current release does not provide:

- IIIF Presentation API manifests, a built-in viewer, collection catalog or login UI.
- IIIF Authorization Flow, automatic token/JWKS discovery or built-in application permissions.
- Upload/ingestion, image editing in storage, automatic pyramid generation or format normalization.
- 16-bit/HDR delivery, arbitrary EXIF orientation normalization, animated image delivery,
  WebP input, or TIFF/JPEG 2000/GIF/PDF output.
- Arbitrary Lua image-processing endpoints, untrusted user scripting or a general plugin loader.
- Automatic whole-process memory admission, a distributed cache or automatic horizontal scaling.
- Unqualified compatibility with an earlier SIPI deployment: check source mapping,
  Lua contracts, supported formats and operational behavior when migrating.

These are current scope boundaries, not promises of future implementation.
