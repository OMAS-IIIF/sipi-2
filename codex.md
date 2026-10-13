# SIPI-2 project context

## Purpose and current state

SIPI-2 is a C++ IIIF Image API 3 server for GLAM image delivery. It serves supported
TIFF, JPEG 2000/HTJ2K, JPEG and PNG sources from local files or S3-compatible storage,
with JPEG, PNG and WebP output. It implements the documented Level 2 operations,
color/alpha handling, Lua access policies, watermarks and reduced public resolution.
Supported pixel/layout subsets matter: consult the [feature catalog](docs/features.md),
not a container extension alone. No upload/import API, original-download API,
streaming encoder response or application login service is implemented.

CMake defines version **0.1.3**. The remote is `OMAS-IIIF/sipi-2`; the documented
published image is `lrosenth/sipi-2:0.1.3`. Matching version strings are not proof of
source/image identity. Release preparation is active; no new release or visibility
change is implied. The [readiness review](docs/release-readiness.md) lists remaining
gates; the [release work record](docs/public-release-plan.md) preserves evidence.

The established delivery feature freeze remains the default: prioritize correctness,
security, documentation and release verification. New formats, upload, streaming
and speculative optimizations need an explicit new scope. Earlier experiments and
plans do not override this direction. Historical context is preserved in the
[pre-consolidation archive](docs/history/codex-before-consolidation-2026-10-13.md);
new technical work belongs at the top of [CODEX_LOG.md](CODEX_LOG.md).

## Architecture and invariants

The request path is HTTP syntax/query validation → bounded worker → Lua policy →
source resolution/version-bound open → metadata and delivery admission → effective
public geometry → authorized result-cache lookup → rendering/encoding. Required
masks are checked before result reuse. Lua runs on image/info/base requests and
GET/HEAD, including cache hits. Explicit denial is 403; policy failures do not
become grants. HTTP responses retain `Cache-Control: no-store`.

| Boundary | Responsibility and key files |
| --- | --- |
| Startup/configuration | `src/main.cpp`, `src/CommandLine.*`; typed settings, environment snapshots, explicit TOML and startup validation |
| HTTP | `src/HttpServer.*`, `src/http/HttpProtocol.*`, `IiifInfo.*`, `ImageUri.*`; routing, queries, CORS, public identity and responses |
| IIIF syntax/geometry | `src/iiifparser/`; owning value types and pure parsing/geometry, separate from codecs/storage/HTTP |
| Delivery | `src/http/IiifWorker.*`, `DeliveryPolicy.*`; request admission and ordered composition of storage, policy, caches and rendering |
| Storage | `src/storage/`; `ByteSource` positioned reads, local RAII handles and version-bound S3 ranges; `src/image/SourceResolver.*` owns mapping/containment |
| Codecs | `src/imgformats/`; TIFF/libtiff, JPEG/libjpeg-turbo, PNG/libpng, JPEG 2000/Grok and output encoders; third-party types stay behind adapters |
| Pixel processing | `src/image/`; owning raster types, resize/rotation, alpha, grayscale/bitonal, watermarks and restricted public variants |
| Color | `src/image/SrgbProfiles.*`, `GrayConversion.*` and `WatermarkColor.*`; LittleCMS and embedded profiles with explicit preservation/conversion contracts |
| Lua | `src/preflight/`; strict typed decisions, isolated states, JSON/HTTP/JWT, explicit config/secrets and bounded shared TTL cache |
| Caches | `src/cache/`; rendered identity, SQLite/file results, metadata, masks and reduced public rasters |

Configuration resolves CLI > environment > explicit TOML > defaults. No automatic
config discovery or hot reload. Settings, Lua text and secrets are startup snapshots.
File/config/script paths use process cwd; relative Lua image/mask sources use
`iiif-root`. Default source mapping uses the identifier only, not URL prefix.
Trusted absolute Lua source overrides may intentionally bypass the local root;
S3 tables select a key in the configured backend, never arbitrary credentials.
Public origin/proxy mount come from `public-base-url`, not forwarded headers.

Lua 5.4 uses a fresh state for initialization/every evaluation. Only explicit
`sipi.cache` values are shared between evaluations; globals are not persistent.
Limits and restricted libraries do not make operator scripts an untrusted-code
sandbox. HTTP/cache helpers are request-only; JSON/JWT and configuration have their
own contracts. Native Lua callback boundaries must respect longjmp and C++ lifetimes.

The worker pool has 1–32 workers (default detected CPUs clamped to 1–4) and 64
waiting jobs. Drogon's HTTP loop remains separate; `/health` is liveness only.
Workers, codec handles and shared immutable snapshots have explicit lifetimes;
stop discards queued work and joins active jobs. No immediate I/O cancellation or
whole-process memory quota is promised. See [worker ownership](docs/worker-pool.md).

All decoding for one request uses a consistent opened source/version. Local inputs
must not be overwritten in place while read; S3 uses version/conditional reads
and bounded range reuse. Metadata/pixel work stays outside cache locks where possible.
One process owns each persistent cache directory; encoded bodies and SQLite are
regenerable and separate from original media. Cache failures permit uncached delivery;
source or policy failures do not. Key identity includes source version, effective
variant, operations, encoding and watermark policy. There is no distributed cache.

## Important completed implementation steps

The modular parser/configuration foundation grew into local/S3 IIIF delivery,
then full documented transforms, color/alpha behavior and three output encoders.
Lua authorization precedes every result reuse; restricted resolution changes the
entire public coordinate system and requires stable distinct public/original URLs.
Watermarks fail closed when their required mask is unavailable.

Later work added bounded parallel delivery, persistent and memory caches, generic
Lua HTTP/JWT/configuration/shared caching, configurable JPEG encoding backed by
libjpeg-turbo, and HTJ2K input through the existing Grok adapter. Source/version
handling remains shared across codecs. Dated benchmarks, sanitizer runs and
external-validator results are evidence for their recorded revisions/workloads,
not universal performance or certification claims.

Public onboarding now includes README, feature catalog, native build/configuration,
Docker, Lua and operations guides with synthetic images and runnable policies.
No private benchmark original is required by ordinary onboarding/native tests.

## Build, validation and release workflow

Use C++20 without compiler extensions, target-based CMake (minimum 3.24), Conan 2
and Ninja. Grok requires a compiler with C++23 support internally. Dependency versions
and revisions live in `conanfile.txt`/`conan.lock`; its recipe is under
`conan/recipes/grok/`. Follow [building](docs/building.md) for profile export,
Debug/Release and CLion. Generated presets/build trees are local, not committed.

Run focused existing CTests for changed contracts; use broader codec/HTTP tests
when behavior or dependencies change. Tests generate normal fixtures. S3 tests
need the separate MinIO setup. `make test` additionally needs Docker and the
external validator; it is not a bare-checkout command with no prerequisites.
See [Makefile workflow](docs/makefile.md) and [dependency updates](docs/dependency-updates.md).
`make docker-local` does not publish; **`make docker-build` does publish** after
verification. Do not invoke publication merely to test a documentation example.

Existing native Debug 0.1.3, published native ARM64 image and isolated Caddy proxy
checks support the public examples. AMD64 manifests were inspected; historical
cross-platform checks have their own evidence. A fresh isolated checkout/cache
build, current release-platform acceptance and exact source/image provenance remain
open release gates. No current test-count assertion substitutes for those gates.

## Working conventions and preservation

Use English in code/docs and German with the owner. Prefer small, explainable
increments, composition, value semantics, standard containers, RAII and the rule of
zero. Use `unique_ptr` for exclusive ownership and `shared_ptr` for actual shared
lifetime. Keep borrowed views within owner lifetimes and document ownership,
threading, side effects and non-obvious invariants. Avoid speculative registries,
frameworks and thin abstractions without a concrete reusable boundary.

Keep changes proportional and synchronized with public contracts. Preserve existing
user work. Update this concise file for strategic changes and prepend dated entries
to `CODEX_LOG.md`; do not rebuild a chronological implementation diary here.
Public guides are indexed in [docs/README.md](docs/README.md). The sibling `cserve`
is historical migration material, not a build dependency or a mandate to copy its
legacy build system. OLDAP policy stays in `examples/oldap/`, outside generic C++.

Never wholesale-delete `build/`: it contains private prepared images and
non-reproducible benchmark evidence as well as build outputs. Keep the two excluded
originals (`sample-tiff-02.tif` and `swiss-map-raster500_500_komb_25_2056.tif`) local
and ignored, including derivatives. Their approved public-main history rewrite
completed in October 2026; do not reintroduce the old history or publish private
backup refs. Attribution/license choices for the other seven originals remain open.
See [corpus provenance](docs/archive-test-corpus.md#source-attribution-and-publication-review-2026-10-13).

Selected wordmarks are in `logo/`. `assets/branding/` contains retained design
explorations and additional icon/mono assets; duplicates are not authorization to
delete all drafts. Preserve raw audit/benchmark provenance; any public redaction
must be explicitly labelled, not silently presented as original measurements.

## Next steps

1. Resolve the concrete publication gates in the [readiness review](docs/release-readiness.md):
   asset attribution, maintainer/security channels and dependency notice inventory.
2. Complete contributor/release/migration entry points and current architecture
   documentation without importing outdated chronological claims.
3. Verify a fresh checkout with isolated generated state and record build/platform
   evidence, then bind the intended source revision to the release artifacts.
4. Review publication scope and perform release/push/visibility actions only when
   requested. Production DNS/TLS, permissions and service lifecycle remain deployment checks.
