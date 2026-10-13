# Changelog

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
