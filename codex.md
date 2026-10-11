# SIPI-2 project context

## Preflight operator diagnostics (2026-10-11)

Startup and runtime logs now include readable error categories and Lua diagnostics
with source location. The Lua boundary masks exact configured secrets and incoming
credentials, escapes controls and bounds message length; HTTP errors stay generic.
Script authors must avoid logging transformed secrets or remote response bodies.
Cache loaders preserve their original Lua error/source location across C++ cleanup;
native cache failures retain fixed reasons such as wait deadline or capacity.
Waiters do not inherit the loader request’s diagnostic. See docs/lua-configuration.md
and docs/lua-cache.md for limits; a rebuilt image is needed for deployment.

## Controlled JPEG performance follow-up (2026-10-08)

The archive runner now supports `--jpeg-comparison`: retained IJG 9f at 90/4:4:4,
libjpeg-turbo 3.2.0 at 90/4:4:4, and the same turbo binary at 90/4:2:0. Existing
four-server mode remains available. Explicit image arguments resolve to immutable
IDs; SOF factors are checked before timing. Shared container helper optionally
passes JPEG flags only to SIPI; the historical image receives none. Five runner
regression tests pass. No production/runtime/default change in this step.

Completed 144 trials, 1,092,270 verified measured responses, 2,160 decoded references,
216 info checks and 432 exact native PNG checks. Matrix and balanced URL counts
are complete; hashes stay stable across repetitions/cache modes. All 60 distinct
4:4:4 JPEGs are byte-identical between IJG and turbo in this corpus; PNG bytes and
ICC are unchanged across all variants. Prepared source hashes remain unchanged.
Native ARM64, 4 CPU / 4 GiB quotas, 8-client rendering median: TIFF→JPEG 485.3→643.0
requests/s (about +33%); JP2→JPEG 68.4→69.0 (overlapping ranges, no clear gain).
Turbo 4:2:0 yields 657.2/69.4 and about 15% smaller JPEGs; keep 90/4:4:4 defaults.
Cache rates measure serving, not encoding; no new competitor ranking is inferred.
See docs/benchmarks/2026-10-08-jpeg-backends.md and companion JSON; raw logs,
commands, source snapshots and audit exporter are in build/archive-jpeg-2026-10-08.
Next: controlled OLDAP setup deployment with a pinned image, auth/viewer checks
and rollback; public IIIF communication should use the qualified measured claims.
Existing OLDAP/MinIO services were untouched; no registry push or deployment.

## libjpeg-turbo backend (2026-10-08)

The requested JPEG backend migration is complete: conanfile.txt/lock pin
libjpeg-turbo 3.2.0 instead of IJG 9f. libtiff explicitly selects the same provider.
SIMD on, static linking, v6b-compatible API/ABI (v7/v8 emulation off), TurboJPEG
wrapper API off. Existing jpeglib.h reader/writer callbacks remain unchanged;
CMake links libjpeg-turbo::jpeg-static. Intel NASM is locked; unrelated package
revisions and Linux-only lock entries were retained. Consumers must be rebuilt
together; this is not a binary swap. Clear stale libjpeg-turbo_DIR in existing
CMake caches after installing dependencies and reload CMake in CLion. Verified
one Conan JPEG archive and matching 3.2.0 headers for Debug and Release.
Render recipe v4 invalidates previous codec results across all output formats
(JPEG decoding can also affect PNG/WebP), including reduced source cache identities.
No database/schema change or manual purge. JPEG quality/sampling remain configurable
with defaults 90/4:4:4; same settings need not produce identical IJG/turbo bytes.
All 70 CTest cases pass per local configuration (Release cache golden vector
retested after its deliberate recipe/digest update); 78 external IIIF checks and
companion WebP probes pass. Local ARM64/AMD64 Docker images built/tested with SIMD,
including actual default/4:2:0 JPEG output and JPEG-to-PNG decoding. Tags:
sipi-benchmark/sipi-2-turbo:verify-0.1.0-{arm64,amd64}; no registry push/deployment.
See docs/jpeg-delivery.md#jpeg-backend-and-rebuilds and docs/dependency-updates.md.
The controlled follow-up is complete; see the section above.

## Configurable JPEG encoding (2026-10-08)

Explicitly requested addition after the performance comparison: server-wide
`jpeg-quality` (1..100, default 90) and `jpeg-sampling` (4:4:4 default, or 4:2:0),
with CLI > SIPI_JPEG_QUALITY/SIPI_JPEG_SAMPLING > TOML > defaults. Settings are
resolved at startup, not per request. src/imgformats/JpegOptions.h is the shared
value type used by ServerConfig, the immutable worker settings, JPEG writer and
cache identity. Cache JpegEncoding aliases this type to prevent divergent settings.
RGB 4:2:0 halves chroma on both axes; gray/bitonal stays one component. ICC and
existing default output remain unchanged. Cache quality remains explicit; color
4:2:0 adds a sampling field, absence retains the old 4:4:4 key. Gray ignores sampling
in keys and output; PNG/WebP and reduced source caches are unaffected. No recipe
bump or manual purge; returning to old settings can reuse retained variants.
Debug/Release builds and focused tests pass, including 490 startup cases and an
HTTP restart test inspecting SOF/DQT markers, per-source precedence and cache
reuse. See docs/command-line.md#jpeg-encoding, docs/jpeg-delivery.md, docs/cache-key.md.
The subsequent backend migration is recorded above. Next: repeat controlled JPEG
benchmarks with documented quality/sampling; production deployment remains unchanged.

## Consolidation workflow (2026-10-08)

Root Makefile now provides version bumps, native build/full test orchestration and
Docker verification/publication. CMakeLists.txt remains the sole version source;
scripts/version.py atomically edits only its declaration without Git side effects.
`make test` runs every registered CTest with MinIO enabled, the external IIIF
validator and native Docker runtime checks; dependencies are explicit, never skipped.
`make docker-build` verifies both amd64/arm64 then pushes lrosenth/sipi-2:VERSION
and :latest through the selected Buildx builder. `docker-local`/`docker-test` do
not publish. scripts/docker_image.py sequences build/check/push and stops on failure.
Runtime images contain stripped executable/shared libraries and optional OLDAP Lua
examples, no C++ sources/build toolchain; media/Lua/configuration are mounted at
runtime. docker/compose.example.yaml is a generic example, independent of OLDAP
setup. See docs/makefile.md and docs/docker.md. Existing development MinIO data is
preserved; smoke containers/volumes are uniquely owned and removed by tests.
Verified: all 69 CTests, 78 external IIIF checks and companion WebP probes; real
amd64/arm64 runtime/OLDAP/generic-mount checks and the Compose example pass.
No registry push, version bump or deployment was performed for this consolidation.

## Comparative performance baseline (2026-10-08)

Opt-in `benchmarks/server_compare.py` now runs isolated, native ARM64 Docker
comparisons against Cantaloupe 5.0.7, IIPImage 1.3 and a digest-pinned Triplet.
Same synthetic pyramidal TIFF; 1/8 clients, JPEG/PNG, caches off/on, three
repetitions, independent decoded references and per-response byte checks.
96 timed trials / 1,024,576 measured responses passed. Results are workload-specific:
IIPImage leads rendering; SIPI-2 leads cached PNG; Triplet leads cached JPEG with
smaller/more lossy output. JPEG sampling/quality, PNG compression, reduced TIFF
sampling and cache types differ explicitly. Retain trial ranges: some first trials
were slow without an isolated cause. Do not infer a universal server ranking.
See docs/server-comparison.md and docs/benchmarks/2026-10-08-servers.{md,json}.
Raw timings/configs/logs/runner snapshot: build/server-compare-2026-10-08/.
No product optimization/deployment change. The real-master extension below uses
separately documented JPEG2000 decoders, preserving feature freeze.

## Prepared real-image corpus (2026-10-08)

Offline `prepare-archive-image` (EXCLUDE_FROM_ALL CMake target) plus
`benchmarks/prepare_archives.py` normalize the user TIFFs at full input precision
with LCMS and generate controlled RGB8/Gray8 pairs. Original files remain unchanged.
Six pairs include a subsequently supplied 229 MP Swiss palette GeoTIFF, expanded
losslessly to RGB. Tagged RGB16 -> sRGB8; Gray16 Gamma 1.8 -> Gamma 2.2 D50;
pharch_01 alpha becomes uniformly opaque at 8 bits, then is omitted. Target ICC
matches across reference/LZW 256px main-IFD pyramid/lossless Grok JP2 (1024px tiles,
six resolutions, RGB reversible MCT). Independent Pillow/OpenJPEG pixel checks,
34 pyramid levels, three tool regressions and 48 HTTP checks pass.
Canonical generated corpus: build/archive-images/; audit and reproduction:
docs/archive-test-corpus.md, docs/benchmarks/2026-10-08-archive-{corpus,http}.json.
No new server input support, import route or format-performance claim.
The default admission policy rejects the 18 MP JP2 because full/max exceeds the
conservative decoding budget. Existing max-width/max-height=2048, max-area=4194304
admits all six pairs without reducing source coordinates; this is test configuration,
not a production change. The real-corpus comparison retains these bounds; the
separate synthetic comparison runner still uses its original 4096px source.

## Real-image comparison workflow (2026-10-08)

`benchmarks/archive_compare.py` reuses the comparison container/timing functions
for all six TIFF/JP2 pairs, JPEG/PNG output, five native/fractional/overview requests
per image, 1/8 clients, caches off/on and three repetitions (192 timed trials).
Every client completes whole balanced rounds; references decode independently,
native PNGs must match in common sRGB, and all timed responses retain their own
server's reference bytes. Cantaloupe's OpenJpegProcessor invokes an external
OpenJPEG CLI; benchmarks/docker/CantaloupeOpenJPEG.Dockerfile prepares a local
derivative with that dependency. TIFF remains on Java2dProcessor. Other decoders:
SIPI Grok, IIP OpenJPEG, Triplet libvips/OpenJPEG. This compares complete pipelines,
not identical codec implementations or equal-distortion JPEG. Cache types and
encoder settings still differ as documented. No source or delivery code changes.
See docs/archive-comparison.md for reproduction, output bounds, ICC diagnostics
and benchmark limitations. Three workload/accounting regressions run through
benchmarks/test_archive_compare.py without Docker or the private corpus.
Completed: 192 timed trials / 1,201,230 verified responses, 2,880 serial decoded
outputs and 288 corpus info checks. Every trial is balanced; source hashes and
per-server reference bytes are unchanged across cache modes/repetitions. Results:
docs/benchmarks/2026-10-08-archive-servers.{md,json}; raw evidence and measured runner
snapshots: build/archive-compare-2026-10-08/. SIPI TIFF-to-JPEG is ~7.2x its JP2 rate
in this mixed workload; SIPI JPEG rendering is ~1.76x/1.91x the configured Cantaloupe
TIFF/JP2 rates. IIP leads TIFF rendering; JP2 rates are closer. Retain encoder/cache
and colour caveats, including Cantaloupe's frequently omitted ICC profiles.
No universal ranking or new optimization follows automatically. All owned containers
were removed; existing OLDAP/MinIO services remain unchanged. Next: discuss measured
tradeoffs and whether to align JPEG settings before the deferred optimization work.

## Current milestone: delivery feature freeze (2026-10-08)

The agreed reduced public image feature is implemented. Lua Allow::resolution has
required positive uint32 max_width/max_height. Stable public/original URLs are
chosen and protected in Lua; C++ remains project-neutral. Metadata/info/geometry
use the fitted public canvas, independent of global output limits. The worker
materializes one complete reduced source via existing TIFF/J2K level selection or
JPEG/PNG reads before watermark/crop/resize/rotation/quality. VariantCache retains
immutable reduced rasters (16 entries/64 MiB by default, CLI/env/TOML configurable);
resident data regenerates after restart. Persistent encoded results have separate
restricted identities and still survive restart. Every hit follows authorization
and a fresh source open/version check. See docs/restricted-resolution.md and
examples/restricted-resolution.lua. Key changes: preflight/LuaPreflight.*,
http/IiifWorker.*, http/DeliveryPolicy.*, cache/VariantCache.*.

External feature-freeze validation passed on 2026-10-08: 31 level-2 plus eight
selected optional upstream tests on each original/restricted service (78/78);
independent WebP signature/full decoding also passed. The unmodified pinned
validator has a Python-3 WebP bytes/text bug, documented with initial diagnostics
in docs/external-validation.md. No server code changed for validation.

Feature freeze is now the roadmap: fix correctness/security issues, consolidate
historical documentation/support matrices,
finish the color-management handbook and prepare the first release. Do not resume
new formats, upload or speculative performance work without explicit direction.
Older chronological sections below describe development history and may contain
superseded next steps; this milestone takes precedence.


- Watermark foundation (2026-10-08): image/Watermark.* prepares a centered, proportionally fitted gray8 coverage mask and composites one destination-encoded color with opacity over full rasters or native-grid crops. Pure owning C++ functions, straight alpha, explicit budgets; HTTP/Lua delivery is active. Reduced-grid mapping clips rational canvas edges and samples the original small mask directly (area reduction/linear enlargement), without full-resolution expansion, with crop consistency tests; J2kReadPlan exposes full selected-level dimensions. PngReader::read_png_mask now loads untagged 1/2/4/8-bit gray coverage PNGs (including Adam7) through ByteSource with 16 MiB input/pixel defaults; alpha, palette and color declarations are explicitly rejected. WatermarkColor converts a single sRGB swatch into the destination ICC/encoding using LittleCMS, with existing untagged RGB/gray assumptions and no image retagging. Typed Allow::watermark now accepts required local/S3 source, width_fraction, opacity and sRGB8 color; malformed fields fail the Lua contract. IiifWorker resolves local/S3 masks, validates them before info/base/HEAD and result hits, and applies them on the selected decoder grid before resize/transforms/quality. Missing/invalid masks fail closed with 500. WatermarkIdentity binds source version, width, opacity, color and recipe into a separate result variant; unmarked serialization is unchanged. Coordinates/service identity remain unchanged, responses no-store. MaskCache now retains shared immutable coverage by freshly opened source identity/version/size, default 64 entries/32 MiB, configurable via mask-cache-max-entries/max-bytes with usual precedence; either zero disables. PreparedWatermark shares pixels without copies. I/O/decode stays outside the cache mutex; concurrent cold misses may decode independently. Resident budgets exclude active evicted snapshots/native scratch. HTTP tests prove reuse without result caching, disablement and local/S3 replacement/deletion. Release tile benchmarks now cover local/MinIO, TIFF/JP2, both caches on/off and three repetitions: 427,008 byte-verified responses. Long result-hit phases show roughly 39–41% local and 68–79% MinIO throughput gains from mask caching on synthetic inputs; no cloud-capacity claim. See docs/benchmarks/2026-10-08-watermarks.md. S3 debug diagnostics and Release I/O profiling now show 19 GETs/TIFF tile (18 ≤1 KiB; median 6 B), versus larger JP2 reads. Warm results retain exactly two HEADs and no GET. S3ByteSource now retains up to 16 aligned 4 KiB pages per opened source for small reads; larger reads bypass. Copies share the pinned LRU, small fills serialize, changed-source errors poison retained pages. New openings still HEAD; cached snapshot bytes need no GET until a miss. Actual TIFF GETs fell 19→6 with ~16% extra payload, rendering improved 2.31x over MinIO. Paired JP2 rendering measured ~2.7% lower (cause not isolated); further optimization is deferred under the delivery feature freeze. See docs/benchmarks/2026-10-08-s3-buffer.md and docs/s3-byte-source.md for exact snapshot semantics, limits and tests. See docs/watermarks.md.

Security maintenance: see [dependency update procedure](docs/dependency-updates.md)
for version/lock updates, validation, cache compatibility and deployment.

## Purpose and repository state

SIPI-2 is a new high-performance, versatile IIIF server. It will selectively reuse
understood, useful code and regression fixtures from the sibling `../cserve`
repository. The repository now runs a minimal Drogon HTTP server after resolving
CLI/environment/TOML configuration. GET /hello returns HTML, and /hello/...
echoes the backend URL with HTML escaping. Port defaults to 8080 and is configurable
through --port, SIPI_PORT, or TOML. The listener defaults to 127.0.0.1; listen-address is configurable for containers. The initial executable ran successfully in CLion
according to the owner. A standalone IIIF Image API 3 syntax parser now produces
owning typed image/info requests. The HTTP catch-all maps invalid syntax to 400 JSON and applies configured
preflight to valid requests, returning information JSON or JPEG/PNG for supported tiled or strip-based gray/RGB/RGBA8 TIFF crops; exact /health returns liveness/version JSON. Area-weighted reduction, horizontal mirroring and exact clockwise quarter turns
are integrated; arbitrary-angle rotation and socket streaming remain pending. A standalone TIFF metadata reader
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
requires evaluation. Grok header reading and the local MinIO Docker setup are now integrated. Do not copy cserve's legacy
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
- `src/imgformats/TiffMetadata.h`, `TiffReader.h`, `TiffReader.cpp`: shared RAII TIFF metadata and tiled/strip gray/RGB/RGBA8 crop reading.
- `src/image/PixelRegion.h`, `RasterImage.h`: format-neutral pixel rectangle and owning RGB/RGBA8 raster.
- `src/iiifparser/IIIFGeometry.h`, `.cpp`: pure region/size resolution with explicit policy.
- `docs/tiff-metadata-and-geometry.md`: reader limits, rounding rules, composition, feature roadmap.
- `src/iiifparser/IIIFParser.h`, `.cpp`: pure IIIF 3 path parser and owning value types.
- `docs/iiif-parser.md`, `tests/IIIFParserTests.cpp`: parser contract and regression cases.
- `conanfile.txt`: spdlog 1.17.0, CLI11 2.6.2, Drogon 1.9.13 (no Boost/ORM), libtiff 4.7.2, libjpeg-turbo 3.2.0, Lua 5.4.8, CMake generators and layout.
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

Next: measure remote request counts and add bounded range buffering as justified. The first tiled RGB/RGBA8 TIFF crop decoder is
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
handles tiled or strip-based gray/RGB/RGBA8 crops; upload integration remains deferred. Do not introduce a generic codec framework
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

Implemented through `Allow::resolution`; see the current milestone and
[restricted resolution](docs/restricted-resolution.md) for rounding, distinct Lua
service prefixes, in-memory derivative reuse, persistent result identity and
source-version invalidation. All crops use the same reduced raster. Detail limits,
optional upscaling and final output-size limits remain separate policies.

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
16 MiB Lua memory and one-million-instruction budgets apply. JSON encode/decode is implemented in `src/preflight/LuaJson.*`; see
`docs/lua-json.md` for null/container identity, strict mapping and bounded native
conversion. Generic HTTP(S) is implemented in `HttpClient.*`/`LuaHttp.*` with per-evaluation
budgets; `Jwt.*`/`LuaJwt.*` supply generic HS256/RS256 signing and verification. Default repository mapping now uses iiif-root.

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

`read_tiff_region` decodes only intersecting tiles from the first image or an
explicitly selected compatible pyramid directory: top-left unsigned RGB/RGBA8, contiguous samples and explicitly declared alpha.
Unsupported variants fail explicitly. The format-neutral PixelRegion is shared
with geometry via an IIIF namespace alias; RasterImage8 owns RGB pixels and source
ICC bytes. No implicit sRGB conversion. CMake target tiffreader replaces
tiffmetadata; TiffReadError replaces TiffMetadataError for file/codec failures.
The two entry points share per-handle opening/diagnostics and tag/profile reading
in TiffReader.cpp. Output plus one tile is bounded to 256 MiB by default (caller
parameter); libtiff/metadata limits remain separate. Crop bounds must already be
resolved; the decoder validates, never clips. Source-file stability between reads
is the caller's responsibility. Pixel-exact tests cover edges, tile crossings,
lossless compression, corruption outside/inside the crop, and unsupported formats.
Pyramid selection, other orientations/layouts and enlargement remain open; area-weighted reduction is implemented. Initial
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
size policy, and a bounded worker pool, with 64 total waiting jobs; reject overload with 503 iiif_busy.
Parse before dispatch; authorize image/info GET and HEAD alike. Deny returns 403;
Lua/contract/quota failures 500. Resolve and probe the definitive source, read TIFF
metadata, and pass orientation-normalized dimensions to the pure geometry resolver.
Invalid region/size is 400; disabled actual upscaling is 501 upscaling_disabled.
Supported tiled or strip-based gray/RGB/RGBA8 TIFF crops return JPEG or PNG. Unsupported operations/layouts
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
storage as output capabilities. No sizes/tiles/formats yet; pixel-readable sources advertise mirroring/rotationBy90s
via extraFeatures. Neither
sizeUpscaling nor higher profiles may be advertised merely from configuration.
JSON-LD content negotiation, CORS and base-URI redirects remain later increments.

## First pixel delivery (2026-10-04)

src/imgformats/JpegWriter.* / jpegwriter is an independent RGB8-to-JPEG encoder
using the libjpeg API of direct Conan libjpeg-turbo/3.2.0.
Synchronous span callbacks emit <=16 KiB blocks. Quality defaults to 90, baseline
4:4:4; preserve opaque ICC via APP2 chunks, without sRGB conversion or other tags.
Heap-owned codec/error state plus setjmp keeps libjpeg failures within C frames;
sink exceptions unwind normally, are saved, then rethrown after codec abort/cleanup.

IiifWorker resolves geometry, rejects unsupported transforms/formats, decodes only
intersecting TIFF tiles and collects JPEG blocks into a <=64 MiB string. Publish
200 image/jpeg/no-store only after success. TIFF raster + tile limit remains
256 MiB. JPEG dimensions are limited to 65500; codec/response-limit errors are 500.
Top-left, unsigned RGB/RGBA8, contiguous tiled TIFF; explicit alpha only.
Any region syntax works; resolved output dimensions may stay unchanged or shrink independently.
Reduction uses the format-independent AreaResize module in stored channel values.
Initially rotation 0/360 and default/color JPG only; PNG, native gray, mirroring
and quarter turns were added subsequently (see the sections below). Unsupported
operations return 501, except unsupported rotation angles (400);
TiffUnsupportedError distinguishes layout support from corrupt data (500).
HEAD still performs encoding for identical status/headers; optimize later with a
clear validation/content-length contract. No backpressure-aware HTTP streaming yet.

Generated tiled.tif supports manual delivery; sample.tif remains strip-based.
Tests decode HTTP JPEGs and compare actual crop pixels, verify ICC chunks, sink
failure recovery and corruption inside/outside requested tiles. See
src/imgformats/JpegWriter.h, tests/JpegWriterTests.cpp and docs/jpeg-delivery.md.
info.json still omits profile pending per-resource conformance review; do not infer
output support for every metadata-readable source from this first decoder subset.

## Persistent cache design (agreed 2026-10-04; identity, storage and HTTP integration implemented)

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
citations/annotations. Storage implementation/defaults are described in the
2026-10-05 storage section below. Source metadata caching is now implemented in memory; coalesced generation for
multiple workers remains open. No distributed cache, backend abstraction or browser cache policy change.

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

## Area-weighted reduction (2026-10-04)

src/image/AreaResize.h/.cpp implements downsample_area for owning RGB8 images,
independent of storage/codecs/IIIF. Direct box/area weights use uint64_t common-grid
coordinates and double sums, rounding half up once. No enlarged intermediate.
Identity reuses storage; ICC bytes move unchanged. This explicitly averages stored
channel values, NOT linear light, with no sRGB assumption. Future profile-aware
color processing must define its working space before changing this behavior.

Worker now reads the crop, reduces to the geometry's output size and encodes JPEG,
locally or from S3. Each axis may shrink or stay unchanged; enlargement is still
unsupported even when allowed by policy. Resize budget is 256 MiB for input +
output pixels + ICC, excluding capacity/allocator overhead; failure is 500
processing_failed. TIFF crop-tile selection now uses suitable pyramid levels
(see below); nonpyramidal sources still pay the full-resolution crop cost.
Remote range buffering remains separate work.

See docs/area-resize.md. All 17 CTest suites pass; unit tests use an independent
integer-grid oracle, HTTP tests decode reduced JPEGs, and MinIO compares local/S3
outputs. Relevant ASan/UBSan checks pass (dependencies uninstrumented).


## TIFF read planning (2026-10-04)

`src/imgformats/TiffReadPlan.h/.cpp` adds a pure metadata + IIIF geometry planner.
It owns the selected directory, an orientation-normalized integer enclosing crop,
and exact rational offsets/extents within that crop. Choose the smallest-area
compatible level sufficient in both axes using integer cross-products; otherwise
retain full resolution. This is geometry only, not a decoder capability promise.
The worker now removes unsupported candidate layouts/codecs and uses the plan
to decode the selected IFD via the directory overload of read_tiff_region.
Main-image ICC is inherited; selected corrupt data fails without silent fallback.
PixelWindow/PixelInterval in src/image/PixelWindow.h represent the rational crop
independently of TIFF. AreaResize accepts that window, preserving exact pixel
boundary selection and weighting partial edges in stored channel values.
Top-left tiled or strip-based gray/RGB/RGBA8 delivery is supported. Remote range buffering,
additional pixel layouts and color conversion remain next steps.
All 19 suites pass, including real main-chain/SubIFD, HTTP and MinIO pyramid tests.
See `docs/tiff-metadata-and-geometry.md` for the contract and examples.


## RGB8 PNG output (2026-10-04)

`src/imgformats/PngWriter.h/.cpp` adds an independent synchronous block encoder
(libpng 1.6.58, private PNG::PNG dependency). Worker selects JPEG or PNG after the
same TIFF read plan and fractional resampling. PNG preserves RGB8 samples and ICC,
without color conversion or synthesized sRGB/gamma tags. Basic ICC RGB header
checks plus libpng checks reject problematic profiles; no full ICC validation.
Input cap 256 MiB, ICC 16 MiB, axis cap 1,000,000; HTTP encoded cap remains 64 MiB.
Failure discards output and returns encoding_failed; HEAD follows GET processing.
Independent standard-library Python decoding verifies pixels, ICC and PNG CRCs.
See docs/png-delivery.md. Refresh Conan and reload CMake in CLion for the new codec.
RGBA/alpha-aware filtering and PNG transparency/JPEG compositing are implemented below;
repository TIFF target decisions are in docs/iiif-delivery-architecture.md.


## RGBA transparency (2026-10-04)

RasterImage8 in src/image/RasterImage.h replaces the RGB-only type with explicit
PixelLayout::rgb/rgba; rgba always means straight alpha. TIFF accepts exactly one
associated/unassociated alpha sample, normalizes associated values to straight
8-bit RGB and rejects requested associated pixels with color > alpha. Unknown
extra channels are not guessed. Pyramids retain existing compatibility checks.
AreaResize accumulates colors weighted by alpha in double and returns straight
RGBA; zero-alpha reduced output has black hidden RGB. Identity preserves bytes.
AlphaComposite::composite_on_white compacts RGBA to RGB in place before JPEG;
write_jpeg rejects RGBA itself. PNG encodes RGBA directly. Both processing stages
use stored channel encoding, not linear light; ICC remains unchanged.
Memory bounds count 4 channels for RGBA. Tests cover both source alpha modes,
transparent hidden colors, partial/zero coverage, fractional windows, corrupt
pixels outside/inside crops, inherited ICC and local/S3 PNG/JPEG/HEAD delivery.
See docs/alpha-delivery.md. Color management and further source layouts remain
separate work. Cache and orthogonal transforms were added subsequently.

## Strip TIFF and native grayscale (2026-10-04)

TiffReader now decodes intersecting strips as well as tiles, sharing block-copy
logic and bounding output plus one decoded block. Full-width strips can be costly
for narrow crops; pyramidal tiled TIFF remains recommended for local/S3 serving.
PixelLayout::gray adds native single-channel 8-bit grayscale throughout decoding,
area reduction, PNG and JPEG. MINISWHITE is inverted to black-zero internally;
ICC remains in its original GRAY model, with basic encoder header checks.
Native gray supports default/color/gray qualities and advertises extraQualities
["gray"]. RGB-to-gray conversion, gray+alpha, other bit depths/orientations and
color management remain separate increments. See docs/tiff-strips-and-gray.md.

## JPEG 2000 header input (2026-10-04)

Grok 20.4.15 uses a pinned local Conan recipe in conan/recipes/grok because there
is no ConanCenter package. Export it before conan install on fresh machines.
SIPI stays C++20; Grok builds internally as C++23 and ships as a shared CPU core.
No GPU/Rust/bindings/network clients. Include the Grok shared library in deployment.
J2kReader owns codec/cursor per call, borrows the existing local/S3 ByteSource,
contains callback exceptions and copies owning metadata/ICC. One process runtime
uses synchronous codec-local execution per request; all calls finish before static teardown. Input budget is 16 MiB
with 64 KiB buffering, not a codec-allocation cap. Nonzero reference-grid origins
are subtracted when exposing dimensions. HTTP recognizes JP2/J2K bytes and uses
the existing info/geometry functions; valid pixel requests remain explicitly 501.
Next: optimized region/resolution pixel decode into RasterImage8, then cache.
See docs/jpeg2000-input.md for build instructions, tests and limitations.

TIFF repository policy remains top-left unsigned 8-bit gray/RGB/RGBA, contiguous
channels. Import normalizes other orientations and YCbCr/CMYK into supported
repository representations; delivery does not silently add these conversions.

## JPEG 2000 pixel delivery (2026-10-05)

J2kReadPlan selects the coarsest sufficient wavelet resolution in both axes,
using actual reduced grid extents and exact fractional crop windows. J2kReader
requests only the enclosing region through the same ByteSource, checks supported
8-bit co-sited gray/RGB, decodes synchronously and packs RasterImage8 with ICC.
TIFF/J2K then share area resampling and JPEG/PNG output. Raw single-component
streams mean grayscale; ambiguous multicomponent streams need JP2 color metadata.
No palette/channel mappings, alpha, signed/other bit depths, YCbCr or CMYK.
Unknown output format prevents implicit codec color conversion.

One codec-local executor per call keeps I/O and diagnostic callbacks synchronous.
Global handlers are installed once and route warnings/errors to a thread-local
scope. Treat data warnings as fatal (except pinned unknown JP2 box-skipping
diagnostics): Grok may otherwise return success for partial
tile output. Budget packed output plus logical int32 tile samples to 256 MiB;
codec padding/scratch and metadata are extra. Pixel input reads capped at 256 MiB.
Exact pixels, reduced crop/full-resolution-level consistency, corruption isolation,
ICC, limits, unsupported layouts, concurrency and local/S3 output are tested.
See docs/jpeg2000-input.md. Result cache remains the next architectural milestone.

External balloon.jp2/colortable.jp2 testing exposed benign optional uinf metadata
warnings. The pinned unknown-box-skipping warning is allowed; all other warnings
and errors remain fatal. Generated UUID-info regression fixtures avoid depending
on downloaded assets. Revisit diagnostic text classification on Grok upgrades.

## Encoded-result cache identity (2026-10-05)

src/cache/CacheKey.h/.cpp and the independent cachekey target implement step 1:
required opened SourceInfo + effective variant, resolved geometry, normalized
rotation/quality, typed JPEG/PNG encoding, optional versioned watermark and
rendering recipe. No URL/HTTP/auth input; storage and delivery integration stay separate.
Default/color share for RGB; native gray default/color/gray share. 360 and signed
zero normalize to zero; other angle doubles retain exact binary64 identity.
Length-framed named fields, locale-independent integers and SHA-256 (OpenSSL EVP)
make a stable owning key with immutable accessors. Schema v1 golden-vector tests
protect serialization. OpenSSL 3.6.5 is explicitly required (already transitive).
The persistent index keeps serialized identity beside digest for collision checks.

current_render_recipe in CacheKey.h versions all fixed pixel/color/encoder
semantics; bump for output-affecting algorithm/codec changes. JPEG quality and
effective sampling come from the shared JpegOptions value. Future configurable color/encoder
settings need actual fields. Effective variant MUST reflect actual restrictions;
watermark parameters and source/version are explicit fields of WatermarkIdentity. Restrictions are not implemented by this module. Authorization,
geometry and capability checks still precede EVERY lookup. Storage and HTTP
integration are now implemented below. See docs/cache-key.md.


## Persistent encoded-result storage (2026-10-05)

src/cache/ResultCache.h/.cpp provides the resultcache target: one exclusively
locked local directory, fixed cache.sqlite3 (SQLite 3.53.4 via Conan), sharded
hash-named bodies, full serialized-key collision checks and output metadata.
get returns a shared immutable ByteSource handle; put accepts a complete encoding.
A mutex serializes index/file operations without holding a transaction during
encoding or file writing. Active handles pin entries; reads use independent pread.

Writes use same-directory temporary files, fsync/close/rename/directory fsync,
then a FULL-synchronous SQLite commit. Restart removes temporary/orphan bodies and
missing/wrong-size index rows. Foreign/newer database schemas fail without reset.
A failed put disables further admissions until reopen to preserve accounting.
Cache contents are disposable; source repository data is never cache data.

Config: cache-dir / SIPI_CACHE_DIR / --cache-dir, absent disables; relative to cwd.
cache-max-bytes defaults to 1 GiB, cache-target-percent to 90, same normal priority.
Budget includes encoded bodies plus the serialized in-progress write; SQLite and
filesystem overhead are additional. A 64 MiB free-space headroom check supplements
this, not a hard volume quota. Eviction skips pinned bodies, may decline admission;
single bodies above the lower target may occupy the cache alone up to the maximum.
LRU touches batch every 64 hits, before eviction, and best-effort on destruction.

main initializes/reconciles and holds the configured store, warns/falls back if
unavailable. HTTP lookup/fill and worker-local metadata reuse are now implemented
below. Concurrent fills are now coordinated by the worker pool (see below). No browser cache policy
change, streaming/socket tee, distributed cache or generic database abstraction.
See docs/persistent-cache.md and docs/command-line.md. Tests cover real startup,
configuration precedence, restart/abrupt exit, injected publication failures,
recovery, collisions, LRU/hysteresis/pinning, schema rejection and concurrency.


## Authorized HTTP cache integration (2026-10-05)

main -> run_http_server -> IiifWorker share ownership of the configured ResultCache.
Every image request still runs preflight, opens the definitive version-bound source,
reuses/reads its header, resolves geometry and checks current source/operation support.
Then the worker builds the original-variant identity using actual source layout,
Configured JPEG quality/sampling (defaults 90/4:4:4, shared with the writer) or fixed PNG parameters.
Hit metadata must match the output plan; complete bounded reads precede 200. A hit
skips decoding/resizing/encoding. Missing entries render and publish only complete
successful output. Separate cache exception boundaries allow uncached delivery;
source/policy/codec failures retain normal error mapping. No cached info/errors.

GET/HEAD share results; HEAD retains Drogon's body suppression and may fill/read
cache. Responses remain buffered up to 64 MiB with unchanged no-store headers.
Debug logs report cache outcomes without key/source/auth values. The worker pool now serializes same-key fills while rendering different keys concurrently (see below).
Header caching is implemented below; true streaming remains open. Native gray, RGBA, TIFF and J2K
reuse the same result path. Tests cover actual hit logs, alias equivalence, denial,
source override/version changes and deletion, local/S3 replacement, restart, HEAD,
failed encodings, missing/truncated cached files, DB fault injection and disabled/
undersized caches. See docs/persistent-cache.md#http-integration.


## Worker-local source metadata cache (2026-10-05)

src/cache/MetadataCache.h/.cpp provides metadatacache: a bounded LRU list holding
SourceInfo identity/version/size and shared_ptr<const variant<TiffMetadata,
J2kMetadata>>. Defaults are 256 entries and 64 MiB accounted resident storage;
resolved metadata-cache-max-entries / metadata-cache-max-bytes configure these bounds,
with SIPI_METADATA_CACHE_MAX_ENTRIES / SIPI_METADATA_CACHE_MAX_BYTES environment
counterparts and standard CLI > environment > TOML > defaults. Either zero disables
retention. MetadataCacheLimits defines the shared defaults and flows through startup,
server and worker into the cache constructor. Accounting includes
key/node estimates and vector capacities, ICC profiles, TIFF nested extra samples
and J2K component arrays; allocator overhead/caller-retained evicted values are extra.
Linear scans are bounded by the small entry limit; no duplicate map index.
An external mutex in IiifWorker now serializes shared-cache operations.

IiifWorker owns the cache and is its sole caller. It runs preflight and opens the
current source before lookup; hits skip signature probe/application metadata reads.
Misses read using that same source, retain only successes, and proceed uncached if
admission fails/exceeds budget. No cached errors/unknown formats. Independently
active without cache-dir, empty after restart. Public info documents still generated
per request. Codec-internal header reads can still occur on pixel decode misses.
Local replacement and S3 version changes cannot match the old source key.

Cached metadata is immutable; TIFF read planning filters a local directory-only
copy instead of erasing shared pyramid levels, with no ICC duplication for planning.
Snapshot ownership survives eviction/destruction. Pool-wide cache access is
serialized under the worker metadata mutex; header I/O stays outside that lock. No SQLite metadata schema, TTL, background maintenance or recipe change.
See docs/metadata-cache.md. Unit and real HTTP tests cover budgets/LRU/ownership,
version/format replacement, failures, authorization, aliases and cold restarts;
existing result-cache and MinIO tests cover the combined delivery path.

## Orthogonal transforms (2026-10-05)

src/image/OrthogonalTransform.* in imageprocessing consumes an owning RasterImage8;
call with std::move. Supports exact gray/RGB/straight-RGBA8 pixel permutations,
preserving ICC and hidden RGB under zero alpha. The worker applies crop/size first,
then horizontal mirroring and clockwise 0/90/180/270/360 rotation, before JPEG alpha
composition/encoding. Identity/mirror/180 reuse storage; 90/270 allocate one extra
pixel buffer and swap axes. Default 256 MiB operation budget includes both buffers
and ICC. No codec/IIIF dependencies or shared state in the processor.
Arbitrary angles now use ImageTransform bilinear interpolation (see below).
Pixel-readable info.json sources advertise mirroring/rotationBy90s/rotationArbitrary; profile remains deliberately absent.
Cache key geometry stays pre-rotation; indexed result dimensions are post-rotation.
Existing key rotation fields suffice; recipe unchanged because prior identity
output is unchanged and newly supported operations had no cached results.
See docs/orthogonal-transforms.md. Unit and HTTP tests cover all permutations,
alpha/ICC, budget failures, TIFF/J2K, cache/HEAD and local/MinIO parity.


## Bounded parallel delivery (2026-10-05)

IiifWorker now owns a fixed joinable thread pool and one globally bounded 64-job
queue. iiif-workers / SIPI_IIIF_WORKERS / --iiif-workers use normal CLI > env >
TOML > default priority, valid 1..32. WorkerLimits.h defines the default as detected
logical CPUs clamped to 1..4 (zero detection falls back to one). The conservative
cap limits memory multiplication; it is not an RSS quota. Larger explicit settings
require operator memory planning. One restores serial execution.
All shared state is ready before threads start; partial startup failure joins
started threads. Owner-only stop wakes all, drops queued jobs and joins active work.
Completion exceptions are contained. No forced cancellation or detached threads.

MetadataCache remains externally synchronized: one short worker-owned mutex around
get/put, header I/O outside locks, immutable snapshots shared across requests.
Configured limits are pool-wide; simultaneous cold reads may duplicate header work.
The persistent cache retains its internal index/file mutex. An additional weak
per-key mutex registry in IiifWorker coordinates lookup/render/publication using
full serialized identities after independent policy/source/capability checks.
Waiters recheck the cache; declined/failed fills permit retries. Registry entries
are pruned on acquisition. Different keys render concurrently; waiting same-key
requests occupy worker slots. No result cache means independent rendering. No new
in-memory response cache or policy/error reuse. Grok keeps synchronous codec-local
execution and thread-local diagnostics rather than nested decode pools.

See docs/worker-pool.md for limitations and configuration. All 34 Debug suites and
selected ASan/UBSan tests pass, including concurrent local/S3 TIFF/J2K delivery.
The direct real-rendering worker test passes TSan. Initial HTTP reports with
uninstrumented Drogon/Trantor disappear with both HTTP libraries rebuilt under
TSan (four parallel HTTP runs clean); other native dependencies remain
uninstrumented. No suppressions or blanket claim about all server paths. Recipe/response bytes
unchanged. Future work: measured concurrency tuning, async waiters if hot-key
contention warrants it, streaming/backpressure and remaining IIIF capabilities.


## Reproducible performance measurements (2026-10-05)

benchmarks/CreateImages.cpp provides an opt-in EXCLUDE_FROM_ALL benchmark-images
CMake target: deterministic 4096px RGB8 tiled LZW TIFF pyramid and lossless tiled
JP2. benchmarks/tile_load.py owns local listeners and measures 1/2/4 workers with
8 persistent clients, 128 adjacent tile URLs across two zoom levels, empty then
populated result caches. Default five repetitions and 128 warm rounds; metadata
warmed, OS caches untouched. Checks response status/MIME/256px dimensions and
per-URL SHA-256 equivalence with serial reference. Records raw request times,
throughput, median/p95, sampled RSS, binary/source hashes and generated summary.
Results/large fixtures stay under ignored build/; no timing pass/fail thresholds.

Release dependencies/build use an isolated Conan output directory, preserving
CLion Debug presets. benchmarks/build_http_tsan.py independently rebuilds pinned
Drogon/Trantor and SIPI with TSan without replacing Conan package binaries; the
other native dependencies stay uninstrumented. Requires local profiles/cached
source trees. Uses isolated CMake target/path adapters matching recipe aliases.

See docs/benchmarks.md and docs/benchmarks/2026-10-05-local.md. On the development
M5 Max (18 logical CPUs, 36 GiB), 495,360 measured responses matched references;
four-worker cold-cache throughput was 2.29x TIFF / 3.85x JP2 relative to one worker.
Warm cache around 15k req/s showed no reliable worker scaling in this local client
setup. Keep default cap four; do not generalize synthetic format performance or
sampled small-tile RSS to whole-image requests. HTTP TSan passed four instrumented
runs; the earlier transport reports disappeared without suppressions. Next useful
measurements: real archival masters, S3 and isolated render-vs-publication costs
if profiling is needed. No production behavior/defaults changed by this step.


## JPEG source metadata (2026-10-05)

src/imgformats/JpegReader.* / jpegreader uses pinned libjpeg-turbo/3.2.0 over ByteSource,
without pixel decompression or pathname reopening. Owning JpegMetadata contains
SOF width/height, logical gray/RGB, progressive flag and reconstructed ICC bytes.
Accept 8-bit gray/RGB/YCbCr, baseline/progressive, EXIF absent or orientation 1.
Reject CMYK/YCCK/other precision and orientations 2..8; ingestion must normalize.
Malformed recognized EXIF/ICC, codec warnings, truncation and budgets are errors.
Parse only bounded IFD0 orientation, never thumbnail pointers or EXIF dimensions.
ICC APP2 assembly requires consistent complete unique chunks; preserve opaque bytes.

Heap-owned libjpeg state/source callbacks follow JpegWriter's longjmp discipline;
storage exceptions unwind within callbacks and propagate unchanged afterwards.
16 KiB prefetch, default 16 MiB cumulative header input, ICC assembly at most
16 MiB extra; codec/node overhead is additional. Stop at first SOS: metadata
success does not validate entropy/EOI or post-SOS markers. Per-call state permits
concurrent local/S3 use. Byte sniffing recognizes JPEG independently of suffix.

The shared MetadataCache variant includes JpegMetadata and charges ICC capacity.
The worker supplies neutral dimensions to existing info/geometry builders, with
normal prefix identity, preflight and source-version checks. Accepted info/HEAD
returns 200 with mirroring/rotationBy90s and native-gray quality capabilities.
Unsupported JPEG headers return 501; malformed data returns 500 invalid_image_source.
JPEG image requests use the existing result cache and full pixel decode pipeline.
Existing profile omission remains the user-approved development boundary. No
rendering-recipe bump because existing pixel output is unchanged.

See docs/jpeg-input.md. Tests cover libjpeg-generated baseline/progressive/gray/
RGB/CMYK, YCCK/12-bit rejection, endian EXIF, ICC chunk validation, input/storage
errors, header-only acceptance, concurrent reads, cache accounting, HTTP/HEAD/
authorization/replacement and MinIO.

## JPEG pixel delivery (2026-10-06)

read_jpeg_image rechecks version-bound header metadata/ICC, fully decodes baseline,
progressive and native-gray/RGB JPEGs into RasterImage8 and requires EOI. The existing
PixelWindow resampler crops/reduces, then orthogonal transforms and JPEG/PNG encoders
run unchanged. No scaled IDCT or JPEG ROI optimization. Default input limit is 256 MiB;
pixels + ICC + estimated multiscan coefficient arrays share a 256 MiB working bound,
with native scratch/markers additional. Codec warnings/truncation fail rather than
return partial output. EXIF/ICC interpretation remains pre-first-SOS only.
Successful results enter the existing persistent cache; existing source formats and
recipe outputs are unchanged. Tests cover pixel-reference equality, limits, corrupt
entropy/EOI, crop/transforms, ICC, parallel reads and local/S3 output parity.
PNG input is implemented below; arbitrary-angle rotation remains a separate step.


## PNG source metadata (2026-10-06)

src/imgformats/PngReader.* / pngreader uses the existing libpng over ByteSource.
PngMetadata owns IHDR dimensions, original color/depth, transparency/interlace and
ICC. Accept legal <=8-bit gray/palette and 8-bit RGB/gray-alpha/RGBA, including
Adam7/tRNS. Reject 16-bit and all eXIf-bearing pre-IDAT headers; no orientation or
color conversion. ICC is validated by libpng and retained; traditional color
declarations are flagged. APNG/CICP/HDR are rejected. Text/suggested palettes are skipped.

Read only through the first IDAT header: no pixel/IDAT CRC/IEND/post-IDAT validation.
16 KiB buffering, 16 MiB default cumulative input/per-chunk allocation limit,
1,000,000 per-axis dimension cap. Warnings/CRCs fail; storage errors propagate;
heap codec state and a narrow setjmp boundary preserve C++ ownership. Native overhead
and ICC copies are additional to limits.

The shared metadata variant accounts for PNG ICC capacity. Worker signature sniffing,
preflight/source versions, geometry and info/HEAD use the existing path. Pixel
capabilities depend on supports_png_pixels; supported requests now use result caching.
No recipe/config/dependency change. Tests cover header subsets, malformed data,
limits/concurrent reads, ICC/cache accounting and local/MinIO information delivery.
See docs/png-input.md. PNG pixel decoding and explicit color/animation boundaries are now implemented below.


## PNG pixel delivery (2026-10-06)

read_png_image rechecks metadata and fully decodes through IEND into gray/RGB/straight
RGBA8. Palette/low-bit/tRNS expansion is lossless; gray-alpha expands to RGBA.
Adam7 combines passes in one zero-initialized raster. Existing crop/reduction,
quarter-turn/mirroring, JPEG/PNG output and persistent cache are reused. Cache layout
includes PNG alpha; source capabilities are checked before lookup. Header success
still does not guarantee pixel validity. Truncation/CRC/warnings fail with no output.

Defaults: 256 MiB pixel input and 256 MiB raster+ICC; header cap remains 16 MiB,
native scratch/metadata copies additional. Reject EXIF/APNG, including post-IDAT;
16-bit and CICP/HDR remain unsupported. Traditional gAMA/cHRM without ICC or explicit sRGB and
gray+alpha with GRAY ICC are metadata-only (501 for pixels), avoiding lost color
meaning or mismatched profiles. ICC is authoritative when present; no gamma/color
conversion or assumed sRGB for untagged data. See docs/png-input.md.

Tests compare independently generated raw samples including real Adam7, gray/alpha/
palettes/tRNS, strict late errors, budgets, HTTP transforms/cache/restart/denial and
local/S3 output parity. No config/dependency or recipe bump; old output unchanged.
Next candidates: explicit color-management support or arbitrary-angle rotation.


## Explicit sRGB delivery (2026-10-06)

RasterImage8 now carries optional srgb_intent (0..3), mutually exclusive with ICC.
PNG sources retain explicit sRGB when no ICC is present; ICC takes precedence,
then sRGB, then traditional gamma/chromaticity declarations. Resampling preserves
this field; transforms/compositing retain the owning raster. No pixel color/gamma
conversion is performed. Untagged sources remain untagged; original ICC is unchanged.

PNG writer emits sRGB plus intent. JPEG writer assigns embedded channel-matching
ICC v4 (sRGB for RGB/composited alpha, sGrey for native gray), preserving intent and
clearing the optional profile ID. src/image/SrgbProfiles.* is linked into jpegwriter.
Pinned CC0 profiles, hashes/license and verification notes live in third_party/compact-icc;
no runtime profile files or new library are required. Gray-alpha sRGB can expand to
RGBA safely; gray-alpha with an arbitrary GRAY ICC remains unsupported.

See docs/color-management.md for the conversion/assignment table, actual sample-changing
operations and remaining limitations. Tests cover every intent/layout, full/reduced/
rotated output, exact ICC bytes, unchanged JPEG scan data and local/S3 parity.
No recipe bump: explicit sRGB source pixel requests were previously rejected; older
supported output is unchanged. Future profile/template or color-algorithm changes
must update the recipe. LittleCMS was used only for local verification, not runtime.


## Arbitrary rotation (2026-10-06)

src/image/ImageTransform.* provides rotated_size (shared HTTP/cache/raster geometry)
and transform_image. Exact quarter turns delegate unchanged; other 0..360 angles
use centered inverse mapping, ceil pixel-edge bounds and alpha-weighted bilinear
stored samples, with transparent-black exterior. Mirror precedes rotation. No
angle snapping, profile transform or linear-light filtering. ICC/sRGB survives.

Added PixelLayout::gray_alpha so arbitrary gray rotation preserves GRAY ICC while
adding transparent borders. Exact transforms, area resampling, PNG encoding and
in-place white composition support this layout; JPEG receives gray after composition.
PNG source decoding keeps its prior subset/expansion rules (no unrelated output change).
General rotation checks input+output+ICC against 256 MiB before allocation. Native
capacity/overhead remains additional. Invalid size/angle/overflow throws TransformError.

Worker advertises rotationArbitrary for pixel-readable sources; result dimensions
come from rotated_size before cache lookup. No cache recipe bump: previous supported
angles are byte-unchanged and new angles had no entries. Future interpolation/rounding/
background changes must bump the recipe. See docs/arbitrary-rotation.md and the color
management table. Tests cover analytic sampling, transparent fringes, GRAY ICC/sRGB,
independent HTTP oracle, restart/denial/cache dimensions and local/MinIO parity.


## Color-managed gray quality (2026-10-06)

Conan now pins lcms/2.19.1; imageprocessing privately links lcms::lcms.
src/image/GrayConversion.* consumes RGB/RGBA after crop/resize/mirror/rotation and
converts to native gray/gray-alpha with Gray Gamma 2.2/D50 ICC v4.3. Embedded RGB ICC
wins; otherwise explicit sRGB or the approved untagged-RGB sRGB assumption. Invalid,
mismatched or unusable profiles fail without fallback. Fixed relative-colorimetric
intent + black-point compensation; alpha copied unchanged. Existing gray is an
owning no-op, retaining profile or absence thereof. Default/color requests do not
invoke the converter. Source objects are never rewritten.

Per-call LCMS context/error state/profiles/transform avoids shared mutation. Budget
covers input/output/source+target ICC (256 MiB), native scratch additional. Target
profile description names Gamma 2.2/D50; fixed creation date and unset profile ID
make output deterministic across fresh processes. Conversion failures map to 500
color_conversion_failed and are not cached. Untagged fallback is debug-logged.

PNG now retains native gray-alpha including GRAY ICC, using the already-supported
raster/writer/compositor layout. Cache key accepts gray-alpha and normalizes native
gray quality aliases. Rendering recipe bumped to sipi-render-v2; old disk entries
cannot match but are not deleted eagerly. info.json advertises gray for supported
pixel sources. See docs/color-management.md, the maintained foundation for a future
GLAM color handbook, including exact stage order, assumptions and limitations.

Tests cover analytical neutral transforms (sRGB vs linear RGB ICC), target gamma/
white point/intent, alpha/native gray, failure/no-fallback, budgets/concurrency,
HTTP/JPEG/PNG/cache/restart/fresh determinism and local/MinIO parity. Next: bitonal
quality, or further IIIF compliance; broad color/HDR/linear-light work remains separate.


## Bitonal quality (2026-10-06)

The worker now accepts and advertises `bitonal` alongside `gray`. After geometry
and the existing grayscale conversion, `src/image/Bitonal.*` composites alpha on
white and thresholds stored samples at 128 in place, producing opaque gray8 with
values 0/255 and retained grayscale color metadata. No dithering/configuration.
PNG is exact two-tone (8-bit encoding); lossy JPEG may decode intermediate values.
Native gray keeps its encoding; the threshold is not a linear-luminance midpoint.
See docs/color-management.md for the full GLAM-facing rationale and limitations.
Existing cache quality keys separate bitonal; no recipe bump because it previously
returned 501. Future threshold/compositing changes require a bump. Packed 1-bit PNG,
adaptive thresholding and broader color conversion remain separate future choices.


## Configurable bitonal dithering (2026-10-06)

`BitonalMethod` now selects threshold (unchanged default) or serpentine
Floyd–Steinberg, through `--bitonal-method` / `SIPI_BITONAL_METHOD` /
TOML `bitonal-method`, with existing precedence. Error diffusion follows gray
conversion and white composition, uses stored samples and two budgeted double
rows, and resets per output raster. Independent IIIF tiles can have pattern
discontinuities: do not promise seamless full-image equivalence. No new IIIF URL
syntax. Worker configuration is immutable; cache serialization adds an explicit
method field only for dithered bitonal, retaining existing keys for threshold and
other qualities. Algorithm/color/operational details live in docs/color-management.md.


## Packed bitonal PNG (2026-10-06)

Both threshold and Floyd–Steinberg now deliver 1-bit grayscale PNG. The worker
explicitly requests `PngBitDepth::one`; internal `RasterImage8` stays unchanged.
PngWriter validates opaque gray 0/255 input before output, packs MSB-first into
one ceil(width/8) scratch row with zero padding, and retains ICC/sRGB metadata.
Default PNG encoding remains 8-bit for other qualities; JPEG is unchanged.
Rendering recipe is v3 to prevent previous encoded cache entries being reused;
this intentionally regenerates all qualities under the shared recipe. Independent
decoder and HTTP tests verify exact pixels, IHDR, odd-width packing, profiles and
both bitonal methods. See docs/png-delivery.md and docs/color-management.md.


## IIIF 3 conformance audit (2026-10-06)

See docs/iiif-conformance.md and tests/iiif_conformance_audit.py.
27/41 diagnostic checks pass; remaining checks include optional links and repeated
CORS probes, not 14 independent mandatory failures. Existing 43 CTest suites pass.
Runtime behavior and the approved missing-profile boundary remain unchanged.
Target level2 after resource eligibility/output limits, base redirects, CORS and
JSON-LD negotiation. Do not publish it globally yet. An orientation-6 TIFF has
info 200/image 501; a 70000x1 PNG has info 200/full JPEG 500. Resolve these contracts
before advertising a profile. Credential-aware CORS and common output limits are
the next concrete decisions. Evidence: docs/audits/iiif-3-2026-10-06.json.


## Resource admission and output limits (2026-10-06)

ServerConfig has OutputLimits: max-width/max-height/max-area and SIPI_MAX_* use
normal precedence. Default width 8192, absent height inherits width, area 8388608;
hard ceilings axes32768/area8388608 fit the current buffered pipeline.
IiifWorker bounds SizePolicy, rejects unsupported layouts on info and images,
plans full/max admission, and validates each request before result-cache hits.
DeliveryPolicy shares supported TIFF level selection with decode and accounts
for TIFF buffers, J2K tiles, full JPEG/PNG decode, cached JPEG coefficient bytes
and conservative processing scratch. Encoder dimension/ICC limits come from
JpegWriter. info reports source dimensions plus effective public limits.
Adaptive max/fit retain one pixel for thin sources, respecting area limits.
Admission501 / request working-limit400 / corrupt-source existing500 remain
distinct. Large masters without affordable full/max can now be refused entirely.
Rotated canvases also have the technical area ceiling; native allocations and
late corruption are not guaranteed from headers. See docs/output-limits.md.
Target is IIIF Image API 3 Level 2 plus optional features; HTTP/profile audit gaps
remain. Existing keys are safe because all policy checks run before cache lookup.

## HTTP compliance and configurable CORS (2026-10-06)

Supersedes earlier missing-profile/HTTP-gap notes: admitted resources now publish
level2; info negotiates JSON/JSON-LD with Vary: Accept, per-resource extra qualities
and profile links. Base URIs reuse info authorization/admission before 303.
HttpProtocol owns pure routing/Accept/preflight helpers; HttpServer applies CORS
outside the encoded cache. CLI/env/TOML cors-origins defaults to *, credentials
false; explicit exact origin lists optionally enable credentials and Vary: Origin.
Wildcard+credentials fails startup. OPTIONS never authorizes pixels. See
docs/iiif-http.md for ambiguous-path precedence and deployment contract.
Focused audit 40/41 passes; optional canonical links and external validator/browser
checks remain next steps. No external certification is claimed.

## Canonical image links (2026-10-06)

ImageUri.* now owns shared public service URI encoding and the pure
canonical_image_uri function. Worker supplies resolved geometry, effective source
dimensions, SizePolicy and native-gray semantics after admission. Uses clipped
crop, policy-dependent max or explicit pre-rotation size, normalized angles and
current quality equivalences. Responses deliver 200 directly and combine canonical
and profile links, including cache hits/HEAD; info advertises canonicalLinkHeader.
No encoded-cache recipe/schema change: links are recomputed for each public
identity, never stored with shared source bytes. See docs/canonical-uri.md.
Focused audit now 41/41; external validator/browser/Caddy validation is next.
Future virtual restricted sources must feed effective identity/dimensions here.

## Consortium validator run (2026-10-06)

Unmodified IIIF/image-validator f05e365a1cb28f8707cbd50466a86d8f611d1ab8 passed
31/31 API3 level2 tests and 8/8 selected optional tests against the official PNG
reference image and temporary loopback server. No runtime changes. Opt-in runner
tests/external_iiif_validator.py owns disposable server/storage and preserves raw
output/dependency/commit/image/binary provenance. See docs/external-validation.md.
Validator checkout/venv live under ignored build; Homebrew libmagic is a test
dependency. Upstream random/shallow optional checks complement project tests.
Remaining interoperability work: actual browser/viewer, Caddy/TLS and cookies;
this successful external validation is not formal certification.

## Configurable bilinear upscaling (2026-10-06)

Actual enlargement now follows existing size-upscaling policy (default false)
and IIIF ^ syntax; info advertises sizeUpscaling only when enabled. resize_image
in image/AreaResize.* delegates pure reduction unchanged, adds linear growth per
axis with area filtering for mixed shrink/grow, alpha-weighted stored-channel
colors and constant filter state. Existing planners use full resolution for growth;
all input formats/local/S3 share the processing path. Limits, authorization and
budget checks remain before encoded-cache access. No recipe bump: previous
successful reductions are unchanged; no enlarged successes existed. See
docs/upscaling.md. Tests cover pixels/alpha, mixed axes, metadata/budgets, HTTP
forms/canonical/cache/restart/limits and four-format local/MinIO parity. Next
requested feature is WebP output, to be handled in a subsequent step.

## WebP output (2026-10-06)

libwebp/1.6.0 is added to Conan/lock without changing existing pins. WebpWriter is
a per-call RAII buffered encoder/muxer, preserving alpha/RGB ICC and converting
GRAY ICC or explicit sRGB gray to RGB sRGB; untagged gray assumes Gamma2.2 D50.
Color HTTP output uses lossy quality85/method4; gray/bitonal lossless quality85,
exact1, no codec threads. Embedded profiles are shared in colorprofiles target
with JPEG. Info extraFormats announces webp. Format-specific 16383 axis limits
are checked before cache; JPEG/PNG policy is unchanged. WebpEncoding fields and
codec/color recipe isolate cache entries; media whitelist expanded without schema
or global recipe change. See docs/webp-delivery.md for budgets/color/build details.
52 Debug suites pass, including local/MinIO parity, pixel/ICC/alpha/RIFF checks,
cache restart and HEAD/canonical/policy checks; focused sanitizers pass. WebP input,
encoder configuration and actual browser/Caddy integration remain separate work.

## Generic Lua capabilities (2026-10-06)

Keep native Lua helpers independent of OLDAP and other deployments; project policy,
API routes, claim meanings and source mapping belong in scripts. `sipi.json`
provides strict JSON encode/decode, null sentinel and explicit empty containers,
using existing JsonCpp with protected Lua/C++ allocation boundaries. Fixed byte,
node and depth limits supplement the per-state Lua budget. Generic HTTP(S) now
uses the existing libcurl dependency, with explicit headers/body, verified TLS,
no redirects/proxies/ambient credentials, four calls and a ten-second aggregate
deadline per evaluation. Sessions reuse connections within an evaluation and
remain isolated across Lua states. Network calls are disabled during initialization
and teardown. See `docs/lua-http.md`. JWT helpers are implemented as described
below. Script configuration/secrets are implemented below; request-context additions are implemented below. Shared token reuse needs a
separate bounded lifetime/concurrency contract because Lua states remain isolated.
Always conclude implementation steps by naming the next proposed small step.

## Generic JWT helpers (2026-10-06)

Pinned jwt-cpp/0.7.2 reuses OpenSSL/JsonCpp; existing dependency pins are unchanged.
`Jwt.*` owns native HS256/RS256 signing/verification; `LuaJwt.*` exposes sign/verify
with explicit algorithm/key, strict options and a shared 32-operation/state budget.
Verification returns claims only after signature, mandatory-by-default expiry,
nbf/iat, optional issuer/type and explicit audience policy checks. An aud claim
cannot be ignored by omitting the audience option. No unverified decode, automatic
key discovery, JWE or revocation/cache behavior. HS256 secrets >=32 bytes; RSA
2048..8192 bits, private PEM for sign/public PEM for verify. Strict 16 KiB tokens,
int64 integer seconds, bounded leeway and exact expiration equality; no chrono
conversion of untrusted timestamps. Shared LuaJson parsing/conversion avoids
value-mapping drift; adapted jwt-cpp traits preserve int64 beyond 2038.
See docs/lua-jwt.md for supported subsets, errors, key ownership and limits.
Independent Python/OpenSSL vectors and output verification complement Lua tests.
Operator-selected configuration/secrets are implemented below. Query/context additions are implemented below. Next: a separate application example script.

## Script configuration and secret snapshots (2026-10-06)

`ScriptConfig.*` separates pure CLI/env/TOML per-name resolution from selected
regular-secret-file reads at startup. Dedicated scriptconfig target has no Lua/HTTP
or codec dependency. CLI flags preflight-config/preflight-secret/preflight-secret-file
are repeatable name=value/path; TOML uses arrays. Distinct environment namespaces
SIPI_PREFLIGHT_CONFIG_*, SIPI_PREFLIGHT_SECRETS_* and SIPI_PREFLIGHT_SECRET_FILES_*
normalize suffixes to lowercase. Lua receives only explicit strings/secret bytes
in fresh sipi.config/sipi.secrets tables before initialization; no process-wide
getenv/filesystem capability. Same-source duplicates fail, selected higher entries
win individually, and overridden files are never read. Empty maps are defaults;
settings without a preflight script fail startup (help/version skip cross-check/I/O).
Limits: 64 combined entries, 16 KiB/value, 256 KiB total, 4096-byte file paths.
Regular-file descriptor checks reject directories/FIFOs; preserve exact binary
bytes/newlines. Restart reconstructs snapshots. Server startup now suppresses raw
Lua diagnostics, matching runtime logging, because script errors can contain keys.
See docs/lua-configuration.md and tests/preflight/ScriptConfigTests.cpp plus
script_config_http.py for precedence, isolation, file and end-to-end redaction tests.
Query parameters and explicit request kind/method are implemented below. Next:
application example policy using the generic JSON/HTTP/JWT/configuration helpers.

## Preflight request context (2026-10-06)

Lua receives query name-to-array values (case-sensitive, decode once, preserve
repeats), explicit route kind image/info/base, and original GET/HEAD method. The
HTTP boundary parses raw queries with bounded size/count and fixed diagnostics;
never use Drogon's flattened parameters for authorization. Empty values remain
present; malformed escapes, controls and empty names/components fail before policy
or cache. Native request data remains owned across worker dispatch. The base 303
preserves the validated raw query; canonical links, info id and cache keys remain
credential-free. Every follow-up request and cache hit still runs policy. No native
OLDAP-specific token priority or permissions. See docs/preflight.md and
tests/preflight_context_http.py. Next: compose a deployment policy example,
including explicit token/cookie/header priority, JWT resource binding and REST
fallback with the generic helpers; restrictions remain a separate feature.

## OLDAP deployment example (2026-10-06)

examples/oldap/preflight.lua and sipi.toml compose generic APIs without C++ OLDAP
logic. Query > Bearer > explicitly named media cookie; invalid supplied credentials
never fall back. Verify media HS256/issuer/audience/expiry, mandatory id binding,
optional matching assetId and numeric permission. Explicit minimum_permission
setting chooses the access threshold (1 preserves the Ruby rule, 2 requires view).
Optional anonymous login then mediaobject lookup uses two bounded HTTP calls per
request, never forwards browser credentials and never caches authorization.
Relative path/id/derived/filename mapping rejects invalid components and ambiguous
arrays; native root containment still applies. All IIIF kinds/GET/HEAD share policy.
Tests use independent tokens and a local API, not live OLDAP. See
docs/oldap-preflight.md for migration differences and configuration. Next: decide
permission-1 semantics and validate against an actual OLDAP deployment; then design
bounded shared anonymous-login token reuse to avoid logging in for every tile.

## Docker/OLDAP local trial (2026-10-07)

Dockerfile builds Linux Release with Conan 2.33.0/C++20 on Ubuntu 24.04, then copies
the executable and Conan shared libraries into a non-root runtime. Library lock
adds Linux-specific libuuid/s2n without changing existing pins. docker/conan-profile
uses Ubuntu's exact Autotools versions via platform_tool_requires and a derived
build lock. BuildKit caches dependencies. Numeric listen-address is configurable
CLI > SIPI_LISTEN_ADDRESS > TOML > 127.0.0.1; existing port configuration is separate.
Image environment opts into 0.0.0.0; canonical URL origin remains explicit.
Sibling oldap-mediaserver/docker-compose.sipi-test.yml replaces only imageserver
with port 8182 and read-only images plus a separate cache volume. Existing Caddy
retains /iiif/3, so public-base-url must contain only the public origin. OLDAP
entrypoint translates dedicated media credentials; anonymous lookup is opt-in and
uses the passwordless unknown pseudo-user, or an explicit password for another account. Base Compose/production remain Cantaloupe for rollback.
See docs/docker.md and sibling docs/sipi-test.md; execution evidence is in CODEX_LOG.md.

## Anonymous OLDAP activation (2026-10-07)

Live API inspection confirms /admin/auth/unknown is a passwordless pseudo-login;
the earlier example's mandatory-password assumption was incorrect. Lua sends an
empty password only for this reserved account when no secret is supplied; other
configured accounts still require a nonempty secret. Prefer login.accessToken,
with legacy login.token fallback. The wrapper omits empty legacy password secrets.
The local Compose .env now opts into anonymous lookup, tested through Caddy against
UtCbbUM5Po4g (2400x1200), including the original ^500 JPEG URL, info, HEAD, warm cache
and invalid-token denial. Every anonymous request checks permission; login-token reuse is now implemented
as described below. Ruby's old delegate cached its unknown login token for a fixed
600 seconds.

## Generic Lua shared value cache (2026-10-07)

Each LuaPreflight snapshot owns a shared C++ SharedCache; copies retain it, new
snapshots/restarts do not. LuaCache exposes get/get_or_load and revision-conditional
invalidate, with JSON-compatible owned copies. Bounds: 256 entries/4 MiB resident
key+JSON bytes, 64 KiB/value, 256-byte keys, 0..3600s TTL (zero shares only the current load), 64 calls/state and a shared
5s wait deadline. Monotonic TTL starts before loading; LRU evicts completed entries
only. One load per key; loader failure wakes waiters, nested loaders forbidden,
no mutex across Lua/HTTP. Protected conversion preserves longjmp/RAII boundaries;
operations disabled outside preflight. No OLDAP code in C++.
OLDAP Lua policy caches login using expiresIn minus 5s (max600s), and successful
anonymous per-image decisions using anonymous_permission_ttl (default600, 0 disables,
max3600). Hits never slide expiry. Denials/errors are not retained; expired grants
never survive API failure. Login loads precede permission loads to avoid nested
cache operations; transient 401 results allow revision-safe renewal outside loaders.
Supplied JWTs bypass anonymous grants. Restart clears all grants; no admin purge
endpoint exists. Permission changes can take the configured TTL to take effect. See docs/lua-cache.md and tests/preflight/SharedCacheTests.cpp.
Real OpenSeadragon captures and warmed URL replays are recorded in
docs/benchmarks/2026-10-07-oldap-viewer.md (reproduce with benchmarks/oldap_viewer.py).
Before permission reuse: local sequential medians anonymous24.34 ms, media token
1.73 ms, direct permission API20.53 ms. After enabling the600s Lua permission cache,
a shorter replay measured anonymous1.38 ms. Next: consider a deliberately
authorized invalidation mechanism if immediate revocation is required; current
policy accepts up to the configured TTL. Live API call counts were not instrumented
by these measurements; fixture tests prove one lookup per concurrent cache miss.
