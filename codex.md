# SIPI-2 project context

## Purpose and current state

SIPI-2 is a C++ IIIF Image API 3 server for GLAM image delivery. It serves supported
TIFF, JPEG 2000/HTJ2K, JPEG and PNG sources from local files or S3-compatible storage,
with JPEG, PNG and WebP output. It implements the documented Level 2 operations,
color/alpha handling, Lua access policies, watermarks and reduced public resolution.
Supported pixel/layout subsets matter: consult the [feature catalog](docs/features.md),
not a container extension alone. No upload/import API, original-download API,
streaming encoder response or application login service is implemented.

CMake defines version **1.0.0**, the agreed first stable release, with planned Git
tag `v1.0.0` and image `lrosenth/sipi-2:1.0.0`. These artifacts are not yet
published; the existing 0.1.3 image is historical. The remote is `OMAS-IIIF/sipi-2`.
The [release notes](CHANGELOG.md) define the 1.x compatibility scope. Matching
version strings are not proof of source/image identity. Release preparation is active; no new release or visibility
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
cross-platform checks have their own evidence. Fresh macOS ARM64 Debug verification
at `54f8e54` now passes: separate checkout/Conan cache, 69 CTests and 92 documented
example checks. Local evidence is in sibling `sipi-2-release-check-state/`; the tested
checkout remains clean. The follow-up also passes both S3 suites, 78 external IIIF tests and a new local
ARM64 Docker build/smoke run. Existing MinIO/validator/BuildKit infrastructure was
reused; see readiness scope. A new local AMD64 image at the same revision also
passes build/smoke under emulation on ARM64; no native-x86 or performance claim.
Deployment checks and exact release source/image provenance remain open.

## Release provenance workflow

`scripts/docker_image.py` now derives OCI source/version/revision labels from
CMake/Git and verifies labels/architecture before smoke tests. Local/test builds
may be dirty and advertise `-dirty`; publish rejects staged/unstaged/untracked
changes and uses one fixed `git archive` for all build stages. Git state is checked
between stages. Each invocation records image IDs, Buildx digests, smoke outcomes
and publication status in ignored `build/releases/`; retain successful release
reports separately. Raw Docker builds use unknown provenance defaults. No Git tag
is created and no old-image provenance is inferred. Final multi-platform publication
still uses Buildx/cache and records its actual digest; independent reproducible
rebuilds are not promised. See [release evidence](docs/makefile.md#image-identity-and-release-evidence).
The clean pre-version-bump snapshot `cffd74d` passed ten workflow regression cases
and both platform container smoke suites (AMD64 emulated), with verified labels
and Git archive contents. This evidence does not certify the pending 1.0.0
artifacts: commit and verify the final release snapshot before publication.

## License scope and artifact notices

Software/scripts/configuration/code snippets are AGPL-3.0-only; documentation
prose is CC BY 4.0. Photographs have file-specific Dölf/Lukas Rosenthaler credits;
logos have separate identification/no-endorsement terms. See LICENSING.md.
Docker captures the actual Conan install graph and collects all host package
licenses, including static/header-only code, into `/opt/sipi/licenses`. Reviewed
supplemental Grok/AWS/XZ notices are tied to exact recipe references and hashes;
dependency changes require a notice review. Ubuntu retains its copyright files
and an installed-package/source-identity list is added. Tests check notice bytes
and failures. Inventory/source URLs are not a corresponding-source bundle:
provide required dependency sources, patches and recipes with final releases.
`make source-package` now archives a clean Git snapshot plus exact Conan recipes,
patches and host-library sources for both Linux platforms. It uses a dedicated
Docker source-export target and Conan cache save/restore without binary packages.
A missing host source fails; the result includes locks, notices and checksums.
`SOURCE_IMAGE_REPORT` additionally verifies the same commit/version and exact
runtime dependency inventories against immutable local image IDs. Outputs remain
local under `build/source-releases/`; publishing/linking them is a separate step.
See docs/source-package.md for prerequisites, restoration and build scope.

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
backup refs. The seven tracked photograph files are CC BY 4.0: Dölf Rosenthaler created
pharch_02 (TIFF/JP2) and sample-tif-01; Lukas Rosenthaler created pharch_01 and
pharch_03 (TIFF/JP2) and confirmed authority for his father's photographs.
See [corpus provenance](docs/archive-test-corpus.md#source-attribution-and-publication-review-2026-10-13).

Selected wordmarks are in `logo/`. `assets/branding/` contains retained design
explorations and additional icon/mono assets; duplicates are not authorization to
delete all drafts. Preserve raw audit/benchmark provenance; any public redaction
must be explicitly labelled, not silently presented as original measurements.

## Next steps

1. Resolve the concrete publication gates in the [readiness review](docs/release-readiness.md):
   maintainer/security channels and final corresponding-source distribution.
2. Complete contributor/release/migration entry points and current architecture
   documentation without importing outdated chronological claims.
3. Complete remaining platform/integration gates using the fresh-checkout evidence,
   then bind the intended source revision to the release artifacts.
4. Review publication scope and perform release/push/visibility actions only when
   requested. Production DNS/TLS, permissions and service lifecycle remain deployment checks.
