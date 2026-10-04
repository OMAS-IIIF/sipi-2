# SIPI-2 project context

## Purpose and repository state

SIPI-2 is a new high-performance, versatile IIIF server. It will selectively reuse
understood, useful code and regression fixtures from the sibling `../cserve`
repository. The repository now runs a minimal Drogon HTTP server after resolving
CLI/environment/TOML configuration. GET /hello returns HTML, and /hello/...
echoes the backend URL with HTML escaping. Port defaults to 8080 and is configurable
through --port, SIPI_PORT, or TOML. The listener binds only to 127.0.0.1. The initial executable ran successfully in CLion
according to the owner. A standalone IIIF Image API 3 syntax parser now produces
owning typed image/info requests. The HTTP catch-all maps invalid syntax to 400 JSON and applies configured
preflight to valid requests, returning information JSON or JPEG for supported tiled RGB8 TIFF crops; exact /health returns liveness/version JSON. Scaling, rotation
and socket streaming are not integrated yet. A standalone TIFF metadata reader
and pure IIIF region/size resolver are implemented and tested together. The HTTP
worker now resolves sources, reads TIFF headers, and validates geometry.
Information output and initial TIFF crop/JPEG delivery are implemented.
An independent Lua preflight evaluator now returns typed allow/deny decisions and
optional source overrides. An optional configured script is loaded and validated
before listening and evaluated on a bounded worker for IIIF GET/HEAD.

## Development approach

- Work in small, explainable increments. The owner and Codex alternate between
  implementation and review; understanding the code is a primary requirement.
- Use CLion for native development, CMake for target-based build configuration,
  and Conan 2 for third-party C++ dependencies. Ninja is the local build executor.
- Prefer composition, explicit ownership, and modular C++ with abstractions only
  where concrete requirements justify them. Use C++20 without compiler-specific
  language extensions as the baseline.
- Prefer standard-library containers and value semantics, RAII, and the rule of
  zero. Use `std::unique_ptr` for exclusive dynamic ownership and `std::shared_ptr`
  only for actual shared lifetime. Non-owning access must respect owner lifetimes.
- Write code and repository documentation in English; communicate in German.
- Document important APIs and non-obvious behavior alongside implementation.
- Update this file for strategic changes and prepend technical changes to
  `CODEX_LOG.md`.

## Migration direction

The existing cserve context records Grok as the intended Kakadu replacement and
Docker as the primary deployment target, with native development and debugging.
Drogon is integrated for basic HTTP; its suitability for codec streaming still
requires evaluation. Grok and Docker have not been integrated in this repository. Do not copy cserve's legacy
ExternalProject build wholesale; adopt libraries and source modules incrementally.

## Key files and next steps

- `src/storage/S3ByteSource.h`, `.cpp`, `docs/s3-byte-source.md`: AWS SDK-backed version-bound reads and MinIO tests.
- `src/storage/ByteSource.h`, `LocalFileSource.*`, `docs/byte-source.md`: version-bound positioned reads, local RAII storage and TIFF callback integration.

- `src/image/SourceResolver.h`, `.cpp`, `docs/source-resolution.md`: root mapping, containment, and manual TIFF/HTTP examples.
- `src/http/IiifInfo.h`, `.cpp`, `docs/iiif-info.md`: format-neutral information document and public identity.
- `src/http/IiifWorker.h`, `.cpp`: bounded worker for optional preflight, sources, metadata, and geometry.
- `docs/iiif-delivery-architecture.md`: agreed delivery order, restricted public variants, and open integration decisions.
- `src/preflight/LuaPreflight.h`, `.cpp`: isolated Lua evaluation and typed policy contract.
- `docs/preflight.md`, `examples/preflight.lua`, `tests/preflight/LuaPreflightTests.cpp`: contract, sample, and regression tests.
- `CMakeLists.txt`: C++20 executable `sipi-2`, standalone `iiifparser` library, and CTest targets.
- `src/imgformats/TiffMetadata.h`, `TiffReader.h`, `TiffReader.cpp`: shared RAII TIFF metadata and tiled RGB8 crop reading.
- `src/image/PixelRegion.h`, `RgbImage.h`: format-neutral pixel rectangle and owning RGB8 raster.
- `src/iiifparser/IIIFGeometry.h`, `.cpp`: pure region/size resolution with explicit policy.
- `docs/tiff-metadata-and-geometry.md`: reader limits, rounding rules, composition, feature roadmap.
- `src/iiifparser/IIIFParser.h`, `.cpp`: pure IIIF 3 path parser and owning value types.
- `docs/iiif-parser.md`, `tests/IIIFParserTests.cpp`: parser contract and regression cases.
- `conanfile.txt`: spdlog 1.17.0, CLI11 2.6.2, Drogon 1.9.13 (no Boost/ORM), libtiff 4.7.2, libjpeg 9f, Lua 5.4.8, CMake generators and layout.
- `conan.lock`: pinned direct and transitive dependency recipe revisions.
- `profiles/macos-debug`: Debug/C++20/Ninja host settings and scoped Trantor warning policy.
- `src/HttpServer.h`, `.cpp`: loopback listener, health/hello routes, IIIF catch-all, and read-only method policy.
- `docs/http-server.md`, `tests/http_smoke.py`: HTTP contract and real-process smoke test.
- `src/main.cpp`: environment snapshot, help/version/error output, and logging setup.
- `src/CommandLine.h` and `.cpp`: typed ServerConfig, owning variant result, and CLI11 loader.
- `sipi.example.toml`: supported configuration example.
- `docs/command-line.md`: source priority, validation policy, and parser limitations.
- `tests/CommandLineTests.cpp`, `tests/data`: permanent CTest loader cases and fixtures.
- `README.md`: build commands, CLion setup, and ownership conventions.
- `codex.md`: stable project context.
- `CODEX_LOG.md`: newest-first technical work log.
- `../cserve/codex.md`: earlier architectural assessment and migration considerations.

Conan generates the `conan-debug` preset for both terminal and CLion builds;
the install command in README explicitly selects Debug, C++20, and Ninja.
Dependencies stay in the user's Conan cache and currently use static libraries.
CMake 3.24 is required for the documented `--fresh` transition from the initial
build without a toolchain. Generated presets and build files are not committed.

## Configuration decisions and next steps

Deployment decision: terminate HTTPS at a reverse proxy; Caddy is the owner's
preferred candidate based on OLDAP experience. SIPI-2 will serve internal HTTP.
Drogon is integrated for the first hello route; suitability for codec streaming
and bounded backpressure remains to be evaluated.
The owner values cserve's codec I/O callbacks that feed both HTTP output and the
cache incrementally. Preserve this capability where codecs support sequential
output; evaluate bounded buffering, cancellation, and cache finalization before
selecting the HTTP streaming integration. Do not assume every codec can stream
without seeking or that complete encoded images must always be held in RAM.

The owner implemented and iteratively corrected the original parser; 30 temporary
review cases passed before the agreed switch to CLI11 and TOML. The loader now
resolves CLI > environment > file > defaults, with an injected environment value
and explicit file selection. CLI11's built-in file reader supports the current
simple TOML schema but is not a full TOML validator. Help/version validate CLI
input then bypass environment/file loading. Unknown file keys fail; overridden
lower-priority values are not semantically validated. Tests cover these boundaries.
Application version 0.1.0 lives in CMake. The standard library owns all input/result
storage; CLI11 stays internal to the implementation. No generic config registry.

IIIF parsing is separate from geometry, codecs, filesystem resolution, and HTTP.
`parse_path` accepts original encoded absolute paths, splits before decoding once,
and returns a variant of image request, info request, or diagnostic. Prefix segment
boundaries are retained; pixel values use checked uint32_t and decimals use finite
double. The model uses variants/value semantics and carries no legacy decoder or
canonicalization state. Bare-resource redirects require a later explicit policy.

HTTP integration uses original encoded paths. Exact /health and the earlier hello
regex take precedence over the IIIF catch-all. A pre-routing GET/HEAD policy returns
405/Allow for other methods; revise it when adding /upload. Drogon's registered
routes are case-insensitive, while IIIF parameter parsing remains case-sensitive.
Health reports liveness, not readiness. IIIF preflight selects/checks a source,
and info generation returns metadata; supported TIFF crops produce JPEG. Debug logs record parsed values.

Next: measure remote request counts and add bounded range buffering as justified. The first tiled RGB8 TIFF crop decoder is
implemented; Grok remains the next format to evaluate. Evaluate streaming/cancellation
separately. Tests cover configuration, the HTTP route, and IIIF syntax. The owner already knows C++; focus on design choices and unfamiliar
modern features. Logging remains synchronous console output; Drogon now provides the initial HTTP layer; codec integration remains open.

The macOS dependency build uses a Trantor-only `-Wno-error=unused-result` flag
because Apple Clang 21 diagnoses future::get() in Trantor 1.5.28. Warnings remain
visible; project code is unaffected. The Conan graph still includes OpenSSL,
but no HTTPS listener is configured. Python 3 is required when BUILD_TESTING is on.

## Image serving and upload architecture (agreed 2026-10-03)

Planned delivery repository formats are TIFF, JPEG 2000, JPEG, and PNG, with an
explicit supported pixel-model/bit-depth/layout contract rather than arbitrary
variants of those containers. Keep serving and upload conversion separate;
neither complete pipeline is implemented yet.

- IIIF serving uses direct format libraries rather than libvips. Parse syntax,
  authorize and resolve the definitive source through optional preflight, then
  read metadata, resolve IIIF geometry/policies, and decode the required pixels.
- TIFF: recognize pyramidal tiled images and select a suitable resolution plus
  intersecting tiles; otherwise use tiled access, then strip access where suitable.
  A strip is a decoding unit, not an arbitrary rectangle. Full-image decoding is
  a fallback for supported layouts that cannot use the optimized paths, not a
  promise to support every TIFF variant. Distinguish pyramid levels from pages.
- JPEG 2000: use Grok with region and resolution selection. Always request the
  optimized path; actual work depends on codestream organization and decoder.
- JPEG/PNG: initially decode the full image, then crop/process it.
- Upload/import: use libvips for broad input support and full-image conversion.
  Processing the entire image need not mean materializing all pixels in RAM.
  JPEG 2000 output goes through Grok with validated upload compression options
  and defaults still to be selected. TIFF output uses libvips, with pyramidal
  tiled TIFF the recommended default; other TIFF layouts can be selected.
  JPEG and PNG are also repository output choices.
- JPEG upload targets must be converted to sRGB as a repository policy. Preserve
  correct profile meaning: assigning an ICC profile is not pixel conversion.
  Missing/invalid source-profile policy, alpha handling, compression
  defaults, and metadata/orientation handling require explicit later decisions.

TIFF reading and pure IIIF geometry are separate libraries. The first pixel reader
handles tiled RGB8 crops; upload integration remains deferred. Do not introduce a generic codec framework
before concrete shared requirements emerge.

### Original preservation and presentation scope (agreed 2026-10-04)

Preserve originals unchanged in the future import workflow, including 16-bit
samples, ICC profiles, and embedded metadata. Provide original-file access through
a separate authorized download, not a fidelity promise for IIIF transformations.
Storage/download integration remains deferred.

Normalize imported images into supported delivery sources, preferably pyramidal
tiled TIFF at full spatial resolution. Start the complete pipeline with RGB8;
target color output is 8-bit sRGB, with alpha, grayscale, and bitonal support added
explicitly. Broad source conversion belongs to import. Direct 16-bit delivery-source
support requires a concrete use case and is not a current requirement.
Color-managed normalization must use sufficient precision before 8-bit quantization
and define missing/invalid-profile behavior. Public detail restrictions remain a
separate variant policy. The existing RGB8 decoder only preserves ICC bytes; it
does not yet convert colors. See docs/iiif-delivery-architecture.md for the contract.

## Restricted delivery variants (agreed 2026-10-03)

For museum access restrictions, reduce the full image to a bounded public variant,
then interpret every IIIF operation in that variant's coordinate system. Its
info.json reports reduced dimensions. Give public and original variants separate
stable service identities; the original remains protected. Per-response size
limits alone do not prevent reconstruction from original-resolution crops.

Initially generate the complete reduced source on demand, using available pyramid
or resolution levels, and cache it for reuse. All crops use the same reduced raster.
Detail limits, optional upscaling, and final output-size limits are separate policies.
Authorize before cache lookup. Internal cache identity includes source version,
effective variant and output-affecting settings, not just the canonical URL.
Cache publication requires successful complete encoding; seek-based encoders need
bounded buffering or temporary storage. None of this is integrated into HTTP yet.

See [the delivery architecture](docs/iiif-delivery-architecture.md) for processing
order, examples, proposed building blocks, and open decisions. Next architectural
steps are defining restricted variants and extending the minimal preflight contract.
Lua evaluation is implemented independently and called by the HTTP adapter. Variant URL conventions,
derivative storage/invalidation, and precise output-limit policy remain open.

## Initial Lua preflight (2026-10-03)

Use Lua 5.4.8 through Conan and a small direct C API integration. LuaPreflight
snapshots script text, then compiles/initializes it in a fresh RAII-owned state for
each evaluation. Independent states make repeated and concurrent calls isolated;
no pool or shared request globals. A protected boundary surrounds allocating Lua
operations; callbacks avoid C++ objects requiring destruction across longjmp.

Input includes the parser's decoded Resource and optional authorization/cookie
headers. A strict plain-table result becomes Allow(optional source) or Deny;
technical failures throw PreflightError. Unknown fields and unsupported restrictions
fail explicitly. Paths are not resolved or opened here. A limited standard-library
subset is available; execution remains trusted-code, not a sandbox. Per-state
16 MiB Lua memory and one-million-instruction budgets apply. JWT/JSON/network
host APIs remain pending. Default repository mapping now uses iiif-root.

## TIFF metadata / geometry decisions (2026-10-03)

The TIFF reader owns libtiff handles with RAII, copies ICC/layout/sample metadata,
and exposes stored plus orientation-normalized dimensions. Discover explicitly
marked compatible reduced main IFDs and SubIFD arrays/trees; do not confuse pages
with pyramid levels. No pixel decoding or EXIF/IPTC/XMP preservation. Metadata
limits and suppressed nonfatal warnings are documented in the module contract.
The pure geometry resolver handles all region/size variants, clips regions, uses
explicit rounding, checks uint32/uint64 bounds and optional width/height/area limits.
Results describe the normalized crop and dimensions before rotation. Codec level
selection, orientation pixel mapping, and rotated raster budgets remain separate.
`size-upscaling` is the only new startup setting, default false, with strict boolean
CLI > SIPI_SIZE_UPSCALING > TOML precedence. Actual enlargement with ^ requires
permission; ^ requests that do not enlarge remain valid. ^max requires an explicit
finite policy limit; deployment limit configuration is still to be agreed.

Target all user-listed IIIF features (see the roadmap table), with only
sizeUpscaling feature-switchable. Do not advertise them in info.json prematurely.
HTTP now applies optional preflight, root-based source resolution, TIFF metadata,
and geometry. Default mapping uses only identifier, never prefix segments.

## First TIFF crop decoder (2026-10-03)

`read_tiff_region` decodes only intersecting tiles from the first full-resolution
image: top-left unsigned RGB8, contiguous three-channel layout without alpha.
Unsupported variants fail explicitly. The format-neutral PixelRegion is shared
with geometry via an IIIF namespace alias; RgbImage8 owns RGB pixels and source
ICC bytes. No implicit sRGB conversion. CMake target tiffreader replaces
tiffmetadata; TiffReadError replaces TiffMetadataError for file/codec failures.
The two entry points share per-handle opening/diagnostics and tag/profile reading
in TiffReader.cpp. Output plus one tile is bounded to 256 MiB by default (caller
parameter); libtiff/metadata limits remain separate. Crop bounds must already be
resolved; the decoder validates, never clips. Source-file stability between reads
is the caller's responsibility. Pixel-exact tests cover edges, tile crossings,
lossless compression, corruption outside/inside the crop, and unsupported formats.
Pyramid selection, other orientations/layouts and scaling remain open. Initial
JPEG encoding and HTTP pixel delivery are now integrated. Evaluate Grok next before expanding a generic pixel model.

## Preflight startup configuration (2026-10-03)

ServerConfig has an optional filesystem preflight_script path, resolved through
--preflight-script > SIPI_PREFLIGHT_SCRIPT > TOML preflight-script > absent.
Selected empty/NUL paths fail; overridden lower-priority values are ignored.
Relative paths always use process cwd. The configuration loader performs no script
I/O; main retains a LuaPreflight snapshot and calls validate before listening.
Validation executes top-level initialization and checks the entry function without
calling it. Errors exit 2 even with logging off; help/version bypass script I/O.
Changes require restart. Validation and request evaluation have Lua memory and
instruction budgets; native operations still need their own timeouts.

## HTTP source/metadata/geometry pipeline (updated 2026-10-04)

src/http/IiifWorker (formerly IiifPreflight) owns the optional script, SourceResolver,
size policy, and one worker, with 64 waiting jobs; reject overload with 503 iiif_busy.
Parse before dispatch; authorize image/info GET and HEAD alike. Deny returns 403;
Lua/contract/quota failures 500. Resolve and probe the definitive source, read TIFF
metadata, and pass orientation-normalized dimensions to the pure geometry resolver.
Invalid region/size is 400; disabled actual upscaling is 501 upscaling_disabled.
Supported tiled RGB8 TIFF crops return JPEG. Unsupported operations/layouts
remain 501 not_implemented.
Information requests return 200 JSON with oriented dimensions and public identity.

ServerConfig iiif_root uses --iiif-root > SIPI_IIIF_ROOT > TOML iiif-root > absent.
Explicit roots must exist as directories at startup; relative roots use cwd.
Default source is root/decoded-identifier, ignoring prefix. Relative Lua source
results now use root, replacing the old cwd rule. Reject traversal and root-escaping
symlinks via canonical component checks. Only explicit absolute script overrides
may bypass root; missing required root is a request error, not a cwd fallback.
Canonicalization and open are separate: repository directories/symlinks must stay
trusted. Once open, the same LocalFileSource serves probing, metadata and pixels;
atomic file replacement is safe, but in-place writes are prohibited.

Drogon strips raw Cookie into its cookie map: expose request.cookies in Lua; retain
optional raw headers.cookie for standalone callers. Responses use no-store and do
not disclose credentials, paths, or script-controlled errors. Shutdown discards
queued jobs and joins current work. Lua quotas cannot preempt native/filesystem
operations. Tests include real symlinks, header/geometry checks with classic TIFF,
BigTIFF and orientation, queue limits, and startup precedence/errors. Generated
manual fixtures live in build/Debug/http-test-data; see docs/source-resolution.md.

## Information documents (2026-10-04)

src/http/IiifInfo builds an owning JSON value from resource identity and oriented
width/height, with no TIFF or filesystem dependency. Worker authorizes and reads
metadata first; information GET/HEAD returns 200 application/json, Cache-Control:
no-store. The id uses validated public-base-url (CLI > SIPI_PUBLIC_BASE_URL > TOML)
or the local listener origin, then separately encoded parsed prefix/identifier.
Proxy-stripped mount paths can be configured. Never trust Host/Forwarded for id.
Parser rejects dot-only resource segments to prevent URL normalization changing id.

Explicit owner-approved development compromise: omit mandatory profile until pixel
delivery supports a genuine IIIF compliance level; current documents are not fully
IIIF 3 conformant. Do not advertise parser support, input codecs or TIFF pyramid
storage as output capabilities. No sizes/tiles/formats/extraFeatures yet; neither
sizeUpscaling nor higher profiles may be advertised merely from configuration.
JSON-LD content negotiation, CORS and base-URI redirects remain later increments.

## First pixel delivery (2026-10-04)

src/imgformats/JpegWriter.* / jpegwriter is an independent RGB8-to-JPEG encoder
using direct Conan libjpeg/9f (already pinned transitively in conan.lock).
Synchronous span callbacks emit <=16 KiB blocks. Quality defaults to 90, baseline
4:4:4; preserve opaque ICC via APP2 chunks, without sRGB conversion or other tags.
Heap-owned codec/error state plus setjmp keeps libjpeg failures within C frames;
sink exceptions unwind normally, are saved, then rethrown after codec abort/cleanup.

IiifWorker resolves geometry, rejects unsupported transforms/formats, decodes only
intersecting TIFF tiles and collects JPEG blocks into a <=64 MiB string. Publish
200 image/jpeg/no-store only after success. TIFF raster + tile limit remains
256 MiB. JPEG dimensions are limited to 65500; codec/response-limit errors are 500.
Top-left, unsigned RGB8, contiguous, three channels, tiled TIFF only; no alpha.
Any region syntax works; any size resolving to unchanged crop dimensions works.
Rotation 0/360, no mirroring, default/color JPG only. Other requests return 501;
TiffUnsupportedError distinguishes layout support from corrupt data (500).
HEAD still performs encoding for identical status/headers; optimize later with a
clear validation/content-length contract. No backpressure-aware HTTP streaming yet.

Generated tiled.tif supports manual delivery; sample.tif remains strip-based.
Tests decode HTTP JPEGs and compare actual crop pixels, verify ICC chunks, sink
failure recovery and corruption inside/outside requested tiles. See
src/imgformats/JpegWriter.h, tests/JpegWriterTests.cpp and docs/jpeg-delivery.md.
info.json still omits profile pending per-resource conformance review; do not infer
output support for every metadata-readable source from this first decoder subset.

## Persistent cache design (agreed 2026-10-04; not implemented)

See docs/iiif-delivery-architecture.md#cache-architecture-agreed-2026-10-04 for the
agreed contract. Start with one local cache per server instance, persistent image
files plus SQLite index. Keep versioned source metadata separate from encoded
response entries (key, relative path, bytes, media type, output dimensions, last use).
Preflight and definitive source-version checks precede metadata/output reuse;
geometry and current policy remain validated on hits. Key includes source/version,
effective variant, normalized operations, encoder/color/watermark settings and an
explicit rendering-recipe version, not just the canonical URL or raw credentials.

Evict by byte usage with a lower cleanup target and approximate, batched LRU;
100/90 GiB is an example, not a default. Budget in-progress writes, skip oversized
entries, protect active readers and coalesce concurrent generation for one key.
Write temp -> complete/close -> same-filesystem atomic rename -> publish index.
Recover orphan/missing files and temp remnants after crashes; cache failures should
permit uncached delivery. Cache survives restart but is disposable, not repository
storage. SQLite/filesystem are not one atomic transaction.

Allow same-ID source replacement via complete-file atomic replacement. Initially
use file identity/size/high-resolution timestamps as a practical fingerprint, not
content proof; later prefer import-assigned source versions. Planning/decoding
now share one open local descriptor, supporting atomic replacement while reads
are in flight; in-place writes remain prohibited. Preserve the option of versioned public identities for
citations/annotations. Implementation details (schema, configuration defaults,
reservations, durability, handle/retry strategy) remain open. No distributed cache,
backend abstraction, changed browser cache policy or cache implementation added.

## S3-ready repository reading (agreed 2026-10-04)

S3 is a firm architectural requirement from the outset, not an assumption to
retrofit after extending local-path readers. HTTP source selection supports local files and S3.
The small ByteSource boundary is implemented (info and offset reads, immutable
identity/version/size, explicit ownership, thread safety and storage errors).
LocalFileSource uses open/fstat/pread with RAII; TIFF uses TIFFClientOpenExt and
independent cursors while preserving storage exceptions across C callbacks.
Source identity uses device/inode; version uses size and nanosecond mtime/ctime,
a practical fingerprint rather than a content hash. S3ByteSource now uses AWS SDK
for C++ 1.11.692 (S3 service plus core dependencies). A shared S3Client hides all
AWS types and owns connections; HEAD captures size/ETag/VersionId, GET uses pinned
versions or If-Match (including mutable null versions). Ranges are capped at 1 MiB
and streamed into caller buffers with length/range/validator checks. Requests use
2s connect/10s HTTP timeouts, no redirects and zero SDK retries. HTTP source
selection supports both backends. See docs/s3-byte-source.md. Inspect Grok I/O
before designing its adapter. SourceResolver returns ImageSource (LocalSource or S3Source); Lua overrides
are either local path strings or {type="s3", key="..."}.

Bound and reuse remote reads, coalesce nearby ranges and limit parallel requests.
Pin one source version throughout metadata/planning/decoding, using VersionId or
conditional reads with a defined retry contract. Include backend/source identity
and version in cache keys; retain the result cache and SQLite locally. Source-range
caching is separate and bounded. Avoid a speculative generic storage framework.

Use a small local MinIO store for early integration tests of ranges, consistency,
replacement, errors and local/remote output equivalence. The optional dev/minio/compose.yaml setup now builds a pinned MinIO community
source commit and stores data in a Docker named volume, with loopback-only ports.
See docs/minio-development.md. Source build, readiness, authenticated TIFF upload,
range reads and persistence across container recreation were verified on Docker
Desktop ARM64. The local bucket sipi-test-images contains tiled.tif. Later
validate production-provider behavior and realistic network latency separately.
Both pyramidal tiled TIFF and Grok JPEG 2000 remain targets. TIFF is the provisional
default; distinguish supported inputs from recommended efficient layouts and from
implemented capabilities. Use COG layout principles without requiring geospatial
metadata or GDAL. Select recommendations through comparable-quality local/S3
benchmarks covering file size, transferred bytes, request count, CPU and latency
with cold/warm caches. A later repository validator should share input rules with
import/delivery. Details: docs/iiif-delivery-architecture.md, repository/S3 section.

S3 verification: offline protocol fault tests run in normal CTest; opt-in
SIPI_TEST_S3 enables disposable local MinIO buckets, replacement/version tests
and local/remote TIFF pixel equivalence. All 16 suites pass with integration
enabled, and S3 checks pass ASan/UBSan (dependencies uninstrumented). AWS/HTTPS
and production credential providers remain unverified; no read-ahead/cache yet.

## S3 credential configuration (2026-10-04)

ServerConfig.s3 reuses S3Config; access_key, secret_key and optional session_token
resolve per field from --s3-* CLI > SIPI_S3_* environment > matching TOML root keys
> absent. Explicit empty/control-containing values fail; final access/secret pair
must be complete and tokens require the pair. All absent leaves the SDK provider
chain intact; AWS_* variables are SDK inputs, not SIPI precedence sources.
Help/version skip environment/file/pair checks but validate CLI field syntax.
CLI11 diagnostics use fixed error categories without raw values; never log config.
Tests cover source precedence, redaction and actual main environment wiring.
No SDK client/network access occurs during parsing. HTTP source selection and
endpoint/bucket configuration are now integrated. Details in
docs/command-line.md#s3-credentials and sipi.example.toml.

## Local/S3 delivery selection (2026-10-04)

--iiif-storage / SIPI_IIIF_STORAGE / TOML iiif-storage selects local (default) or s3.
S3 backend settings s3-bucket, s3-region (us-east-1), s3-endpoint (AWS regional
endpoint when absent), s3-path-style (false) have matching CLI/environment/TOML
priority. S3 default requires a bucket; local default may also configure a bucket
for Lua S3 overrides. One configured backend/bucket and one shared SDK client.

IMPORTANT: The IIIF URL prefix stays outside the default mapping. Project-specific
translation belongs in the preflight script. /museum/subpart/tiled.tif/... selects
local root/tiled.tif or S3 key tiled.tif; request.prefix is available to Lua and
still participates in public identity. No second URL decoding of identifiers/keys.

SourceOverride distinguishes legacy local paths from S3Key. String sources always
remain local, even with S3 default; {type="s3", key="..."} always selects the
configured bucket, even with local default. No script-supplied bucket/endpoint or
credentials. Strict Lua tables reject unknown fields/metatables/coercions. Source
resolution returns owning ImageSource variants; worker opens one ByteSource only
after authorization, then reuses existing TIFF/geometry/JPEG pipeline.

See examples/minio.toml for immediate local MinIO delivery and
docs/command-line.md#repository-selection-local-or-s3, docs/preflight.md and
docs/source-resolution.md for the contract. Opt-in s3-http tests compare JPEG
bytes, info/HEAD, explicit prefix translation, mixed sources and denial with an
unreachable backend. All 16 suites and relevant ASan/UBSan checks pass.
