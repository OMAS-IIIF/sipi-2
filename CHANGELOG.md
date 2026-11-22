# Changelog

## 1.0.2 — 2026-11-23

- A source that changes while it is being read (shrunk local file, new S3
  version) now answers 503 `source_changed` with `Retry-After: 1` instead of
  500 `source_error`; a retry reads the new version.
- A failed result-cache write is logged once at error level with its cause
  (OS or SQLite reason, e.g. `No space left on device`) and the hint that cache
  writes stay disabled until restart. Behavior is unchanged.
- New settings `iiif-queue-limit` (`SIPI_IIIF_QUEUE_LIMIT`, 1–1024, default 64)
  and `webp-quality` (`SIPI_WEBP_QUALITY`, 1–100, default 85). Defaults keep the
  previous behavior; WebP quality is part of the cache identity.
- Workers skip queued requests whose client has already disconnected, checked
  before policy evaluation and again before decoding on a cache miss.
- The Docker image defines a `HEALTHCHECK` (`/opt/sipi/healthcheck.sh`, GET
  `/health` via bash, following `SIPI_PORT`).

## 1.0.1 — 2026-11-22

- Removed the diagnostic `/hello` route. It echoed the request URL and Host
  header as HTML and is not part of the image service. `/hello/...` paths are
  now ordinary IIIF requests and are subject to the configured policy.
- The persistent result cache no longer holds its mutex while writing and
  synchronizing a body. Admission, eviction and SQLite operations still hold
  the mutex, including occasional automatic WAL checkpoints.
  Its SQLite index now uses WAL with NORMAL synchronization; existing cache
  directories are converted automatically on open.
- Lua `sipi.http.request` reuses keep-alive HTTP/TLS connections across
  preflight evaluations through a bounded pool of idle libcurl handles. Only
  transport state is shared; headers, bodies and credentials are not.

## 1.0.0 — Unreleased

First stable SIPI-2 release, prepared for public use. Planned Git tag: `v1.0.0`;
planned Docker image: `lrosenth/sipi-2:1.0.0` for Linux AMD64 and ARM64.
The version change itself does not add runtime features or change configuration.

### Release scope

- IIIF Image API 3 delivery with documented Level 2 operations, including region,
  size, mirroring, rotation and quality handling.
- TIFF, JPEG 2000/HTJ2K, JPEG and PNG input from local or S3-compatible storage;
  JPEG, PNG and WebP output, with documented pixel-layout and color restrictions.
- Color management, transparency, watermarks and reduced public resolution.
- Lua preflight authorization and source mapping, with configuration, JSON, HTTP,
  JWT and bounded shared-cache helpers. Authorization also applies to cache hits.
- Bounded image workers, memory caches and persistent encoded-result caching.
- Native build/configuration guides, container and reverse-proxy examples, and
  an image release workflow that records Git identity and Buildx digests.

See the [feature catalog](docs/features.md) for exact support and limitations,
and the [documentation index](docs/README.md) for operational contracts.

### Licensing and attribution

Software and code examples are explicitly AGPL-3.0-only; documentation prose and
seven credited photographs are CC BY 4.0. Photographs by Dölf Rosenthaler and
Lukas Rosenthaler have file-specific attribution. Logo usage has separate terms.
Runtime packaging now includes dependency license texts, supplemental upstream
notices, checksums and an Ubuntu package inventory. `make source-package` creates
source release archives with both Linux dependency graphs, recipes and patches. See [licensing](LICENSING.md).

### Compatibility and upgrades

Within 1.x, documented CLI options, environment/TOML settings and Lua preflight
interfaces are intended to remain backward compatible. Additive features may
arrive in minor releases; intentionally incompatible public-interface changes
require a major release. Security fixes may tighten validation or reject unsafe
behavior and must be called out in release notes. Internal C++ interfaces, cache
file formats and experimental/historical material are not stable public APIs.

This version bump from 0.1.3 requires no configuration migration. SIPI-2 is not
asserted to be a drop-in replacement for earlier SIPI implementations: review
[native configuration](docs/command-line.md) and the [Lua contract](docs/preflight.md)
before migrating an existing deployment. Pin image versions or verified digests;
keep the existing 0.1.3 image available for rollback.

### Boundaries and release status

No upload/import API, IIIF Presentation API, original-download API or application
login service is included. Format support is limited to the catalogued subsets.

Preparation checks passed on the pre-bump source, including native tests and
both container architectures; AMD64 container execution used ARM64-host emulation.
These results are not final 1.0.0 artifact verification or formal certification.
The final clean commit, release tag, publication date and image digest remain
pending, together with the owner decisions in the
[release readiness review](docs/release-readiness.md).
