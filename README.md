# SIPI-2

<picture>
  <source media="(prefers-color-scheme: dark)" srcset="logo/sipi-2-logo-dark.svg">
  <img src="logo/sipi-2-logo-color.svg" alt="SIPI-2" width="420">
</picture>

SIPI-2 is a C++ image server implementing the **IIIF Image API 3**. It reads images
from local storage or S3-compatible repositories and delivers requested regions,
sizes, rotations and quality variants as JPEG, PNG or WebP. It is intended for
image collections, archives and applications that need interoperable image delivery
with application-specific access policies.

Lua preflight scripts connect delivery to your application's authorization and
source mapping. They can also require watermarks or expose a reduced-resolution
public image while keeping the original behind a separate policy.

**Status:** the first-release delivery feature set is frozen; current work focuses
on validation, documentation and release preparation. Admitted resources advertise
IIIF Image API 3 Level 2. The recorded external validator run passed 78 checks
across original and restricted services; this is validation evidence, not formal
certification. See [scope and limitations](docs/external-validation.md).

## Main capabilities

| Area | Capabilities |
| --- | --- |
| Input | TIFF (tiled, pyramidal or strip-based), JPEG 2000 including HTJ2K, JPEG and PNG, within each reader's supported pixel layouts |
| Output | JPEG, PNG and WebP; `info.json` describes the public image and available operations |
| Image operations | Region selection, reduction, optional enlargement, horizontal mirroring, arbitrary rotation, grayscale and bitonal conversion |
| Color and transparency | ICC-aware processing, explicit sRGB handling, alpha-aware resizing and output-specific transparency behavior |
| Storage | Local repositories and S3-compatible storage through a shared byte-source boundary |
| Policies | Lua allow/deny decisions, source mapping, restricted public resolution and single-color watermarks |
| Lua integration | JSON, HTTP(S), JWT helpers, explicit configuration/secrets and shared TTL caching |
| Delivery | Bounded image worker pool, persistent encoded-result cache and bounded in-memory caches; configured authorization runs on cache hits too |

Support for a file format does not imply support for every encoding in that
format. Pixel delivery is based on 8-bit samples: 16-bit delivery, WebP input,
CMYK JPEG, and automatic EXIF orientation normalization are not supported.
JPEG and PNG sources are fully decoded even for small crops. Output dimensions
and processing budgets are bounded; enlargement is disabled by default. SIPI-2
provides image delivery, not a IIIF Presentation API or an image-ingestion system.
See the [format and behavior references](docs/README.md#image-formats-and-processing)
for exact restrictions, including PNG color declarations and JPEG 2000 layouts.

## Quickstart with Docker

Prerequisites: Git, Docker and `curl`. The published image
`lrosenth/sipi-2:0.1.3` supports `linux/amd64` and `linux/arm64`.

Clone the repository to obtain the small, synthetic example image:

```sh
git clone https://github.com/OMAS-IIIF/sipi-2.git
cd sipi-2
docker pull lrosenth/sipi-2:0.1.3
```

From the repository root, run:

```sh
docker run --rm --name sipi-2-demo \
  -p 127.0.0.1:18080:8080 \
  --mount "type=bind,src=$(pwd)/examples/images,dst=/data/images,readonly" \
  -e SIPI_IIIF_ROOT=/data/images \
  -e SIPI_PUBLIC_BASE_URL=http://127.0.0.1:18080 \
  lrosenth/sipi-2:0.1.3
```

In another terminal:

```sh
curl --fail http://127.0.0.1:18080/health
curl --fail http://127.0.0.1:18080/sample.png/info.json
curl --fail http://127.0.0.1:18080/sample.png/full/160,/0/default.jpg -o preview.jpg
```

The information response describes a **320 × 200** image; `preview.jpg` is
**160 × 100**. You can also open the image URL directly in a browser. Stop the
server with Ctrl-C; Docker removes the container automatically.

This example publishes only a loopback port and uses no Lua policy or persistent
cache. With no policy configured, images resolvable inside the mounted repository
are accessible. For your own collection, change the mount source to its absolute
directory path; files must be readable by container UID/GID 10001. Container
configuration, writable caches, Compose and Lua mounts are covered in the
[Docker guide](docs/docker.md). For public service, configure a reverse proxy for
HTTPS and the correct public base URL.

## Build and run natively

SIPI-2 uses CMake and Conan. The application is C++20; the bundled Grok dependency
requires a compiler with C++23 support. Follow the [native build guide](docs/building.md)
for prerequisites, compiler profiles, Debug/Release builds, tests and IDE setup.

After a Debug build, run the same example without Docker:

```sh
./build/Debug/sipi-2 --port 18080 --iiif-root examples/images
```

Use the same `curl` commands above. Stop any Docker demo first, since both examples
use port 18080. Native operation requires no Docker unless you choose container
or MinIO integration tests.

## Configuration and Lua policies

Settings can come from CLI arguments, environment variables and an explicitly
selected TOML file. Precedence is **CLI > environment > TOML > default**.
No configuration file is searched for implicitly. Start with
[`sipi.example.toml`](sipi.example.toml) and the
[configuration reference](docs/command-line.md).

A configured Lua `preflight(request)` returns an allow/deny decision and optional
source/policy settings. Scripts are trusted operator code, validated and
snapshotted at startup; restart after changing them. Without a source override,
the identifier selects a file relative to the repository root; URL prefix
segments are not automatically filesystem directories.

See the [preflight input/output contract](docs/preflight.md),
[configuration and secrets](docs/lua-configuration.md), and
[source mapping](docs/source-resolution.md). The [minimal policy](examples/preflight.lua)
requires adapting its source path. The [OLDAP integration](docs/oldap-preflight.md)
is a separate example, not a prerequisite for using SIPI-2.

## Documentation and development

The [documentation index](docs/README.md) organizes the operator guides, format
references, Lua APIs, architecture and validation evidence.

Useful development commands from the repository root:

```sh
make help
make show-version
make build          # Requires the Conan/profile setup from the build guide.
make test           # Requires Docker/MinIO and external-validator prerequisites.
make docker-local   # Build and test a local runtime image; does not publish.
```

The [Makefile guide](docs/makefile.md) explains prerequisites and separate release
commands. Dependency versions and revisions are recorded in
[`conanfile.txt`](conanfile.txt) and [`conan.lock`](conan.lock); follow the
[dependency update procedure](docs/dependency-updates.md) when changing them.
Performance measurements are documented with their specific workloads and limits
in the [benchmark references](docs/README.md#validation-and-performance).

## License

See [LICENSE](LICENSE) for the GNU Affero General Public License version 3 text.
Embedded ICC profile attribution is recorded in
[third_party/compact-icc](third_party/compact-icc/README.md).
