# SIPI-2

A high-performance, versatile IIIF server, developed in small, explainable steps.
The current executable resolves startup configuration and runs a local Drogon HTTP
server with HTML hello, JSON health, and IIIF syntax routes. Tiled or strip-based gray/RGB/RGBA8 TIFF-to-JPEG/PNG delivery is implemented; area-weighted reduction is supported; configurable bilinear enlargement is supported; streaming comes later.

## Delivery feature freeze

The first-release delivery feature set is complete; current work focuses on
correctness, regression/conformance validation, documentation and release readiness.
Lua can now restrict the **whole public image** before all IIIF operations, using
separate public/original service URLs. See [restricted resolution](docs/restricted-resolution.md)
for policy, cache configuration and the runnable policy example. No OLDAP deployment
policy is changed automatically.

## Build and run

Prerequisites: a compiler supporting C++20 for SIPI and C++23 for Grok, CMake 3.24 or newer, Ninja, and Conan 2.
Python 3 is used by the HTTP integration test (no third-party Python packages).
Direct dependencies are `spdlog/1.17.0`, `cli11/2.6.2`, `drogon/1.9.13`,
`libtiff/4.7.2`, `libjpeg/9f`, `libpng/1.6.58`, `grok/20.4.15`, `lua/5.4.8`, `jwt-cpp/0.7.2`,
`aws-sdk-cpp/1.11.692` (S3), `openssl/3.6.5` (cache-key SHA-256), and `sqlite3/3.53.4` (result index).
Drogon is built without Boost and ORM support. Its Conan recipe still brings
OpenSSL transitively, even though our listener uses only plain HTTP.
`conan.lock` records their resolved versions and recipe revisions.
For security fixes, follow the [dependency update procedure](docs/dependency-updates.md).

On a new machine, detect a local compiler profile once (already done on the
initial development machine):

```sh
conan profile detect --name sipi-2-local
```

Inspect the detected settings before use. The profile lives in the user's Conan
cache, not in Git; each machine needs its own matching compiler settings.

From the repository root:

```sh
conan export conan/recipes/grok
conan install . -pr:h profiles/macos-debug -pr:b sipi-2-local --build=missing
cmake --preset conan-debug
cmake --build --preset conan-debug
./build/Debug/sipi-2
```

Conan reads `conan.lock` automatically, installs libraries in its user cache, and
generates CMake dependency metadata, a toolchain, and the `conan-debug` preset.
`-pr:h` selects the target environment; `-pr:b` selects the environment for build
tools. The versioned `profiles/macos-debug` profile includes the detected local
profile and selects Debug, standard C++20, and Ninja. It also retains an
Apple Clang 21 unused-result warning in Trantor 1.5.28 without treating it as an
error; the exception is scoped to that dependency. Other platforms need a matching
profile. Linux ARM64 builds and container delivery are now validated through the
[Docker workflow](docs/docker.md).
`--build=missing` compiles dependencies if compatible binary packages are missing.
The subsequent commands configure, build, and run our executable. Re-run Conan
when dependencies or build settings change; ordinary source edits only need a build.

When switching from the original build without Conan, configure once with
`cmake --fresh --preset conan-debug` to discard stale CMake configuration. This
`--fresh` option is why the project now requires CMake 3.24.

Example output (timestamp varies):

```text
[2026-10-02 22:53:00.000] [info] SIPI-2 listening on http://127.0.0.1:8080
```

The process now stays running until Ctrl-C or SIGTERM. Open
[the hello route](http://127.0.0.1:8080/hello) or, for example,
[the URL echo](http://127.0.0.1:8080/hello/path1/path2/id?name=test).
Also try [health](http://127.0.0.1:8080/health) or
[a valid IIIF path](http://127.0.0.1:8080/myproj/id/full/max/0/default.jpg)
(requires a configured repository and matching image). For a generated TIFF/JPEG
example, see [first image delivery](docs/jpeg-delivery.md).
See [HTTP behavior](docs/http-server.md) for route and proxy details.

## Startup configuration

```sh
./build/Debug/sipi-2 --help
./build/Debug/sipi-2 --version
./build/Debug/sipi-2 --config sipi.example.toml
SIPI_LOG_LEVEL=debug ./build/Debug/sipi-2 --config sipi.example.toml
SIPI_LOG_LEVEL=warn ./build/Debug/sipi-2 --log-level info
./build/Debug/sipi-2 --port 9090
SIPI_PORT=9090 ./build/Debug/sipi-2
```

Per setting, precedence is **CLI > environment > explicitly selected TOML file > default**.
The sample file contains `log-level = "info"` and `port = 8080`. `SIPI_LOG_LEVEL`
and `SIPI_PORT` supply environment values. Ports must be integers from 1 to 65535. No config file is searched for
implicitly. `--help` and `--version` skip environment/file loading after validating
CLI arguments. Invalid startup input prints to stderr and exits with code 2.
`--log-level off` suppresses the startup log. The application version is defined
once in CMake (`project(... VERSION 0.1.0 ...)`).

CLI11's built-in reader supports these simple TOML settings; it is not a complete
TOML conformance validator. See the contract before expanding the file format.

## Tests

```sh
cmake --build --preset conan-debug
ctest --preset conan-debug --output-on-failure
```

CTest runs the C++ startup loader tests, including CLI errors, all levels, source
precedence, and file handling. Tests inject environment values explicitly and
read fixtures under `tests/data`; they do not modify the process environment.
The Python HTTP test starts and stops the server in a temporary directory on a
free loopback port, checking health/version, IIIF responses, route priority, URL
escaping/decoding, HTTP methods, and SIGTERM shutdown.
No extra testing framework is required at this size.

## CLion

1. Run the Conan install command above, then open this repository as a CMake project.
2. In Settings > Build, Execution, Deployment > Toolchains, use the local Apple
   Clang compiler and bundled LLDB debugger on macOS. Select
   `/opt/homebrew/bin/cmake` to use the installed Homebrew CMake.
3. Reload the CMake presets if the project is already open. Under Build, Execution,
   Deployment > CMake, enable the imported `conan-debug` profile and disable the
   old manually configured Debug profile. The old profile lacks Conan's toolchain.
4. Select `conan-debug` and the `sipi-2` executable configuration to build and run.
5. Set a breakpoint on the `spdlog::info` line in `src/main.cpp`, start Debug, and
   step through the program. No Conan plugin is required.

## Understanding the starting point

The [startup configuration contract](docs/command-line.md) describes the CLI11
loader, source precedence, supported syntax, and validation policy.

- `conanfile.txt` declares library versions, the `CMakeDeps` and `CMakeToolchain`
  generators, and `cmake_layout` for build directories. No Python recipe is needed.
- `conan.lock` pins dependency versions and recipe revisions, including `fmt` and CLI11.
  It does not pin the machine's compiler or operating-system SDK. Update it
  deliberately when changing dependencies.
- `CMakeLists.txt` defines one C++20 executable with extensions disabled.
  `find_package(spdlog CONFIG REQUIRED)` loads the generated dependency metadata;
  `target_link_libraries(sipi-2 PRIVATE spdlog::spdlog)` supplies include paths,
  compile definitions, and transitive link dependencies to our executable.
- `src/main.cpp` logs through spdlog's default synchronous console logger, which
  adds a timestamp and severity, after applying the resolved log level. It also
  handles help, version, and configuration errors before entering the HTTP loop.
  There is no file logging yet. Drogon/Trantor retain their own framework logger;
  our log-level setting currently controls application spdlog messages.
- Conan generates `CMakeUserPresets.json` and files under `build/Debug/generators`.
  These contain local paths and are ignored by Git; regenerate them with Conan.
- `src/HttpServer.cpp` registers health, hello, and IIIF routes and runs the loopback listener.
- `.gitignore` keeps generated builds and local IDE settings outside version control.
- `codex.md` records project decisions; `CODEX_LOG.md` records technical changes.

Prefer standard-library values and containers for ownership (`std::string`,
`std::vector`). Use `std::unique_ptr` when a dynamically allocated object needs
one owner, and `std::shared_ptr` only when shared lifetime is required. References,
raw pointers, `std::span`, and `std::string_view` may provide non-owning access;
their lifetime must not outlast the owner. Resource cleanup follows RAII: an
owning object's destructor releases its resources. Avoid manual `new`/`delete`
in application code and prefer the rule of zero for application types.

## IIIF syntax parsing

The standalone C++20 parser in `src/iiifparser` handles IIIF Image API 3 image
and `info.json` paths with typed, owning results. The HTTP catch-all returns 400
for invalid syntax; valid requests enter optional preflight and source checks,
with supported tiled or strip-based gray/RGB/RGBA8 TIFF crops delivered as JPEG or PNG. See [the parser contract](docs/iiif-parser.md) for syntax, decoding, and
the boundary between parsing and image geometry. Its regression tests run with CTest.

## TIFF metadata and geometry

Conan now also supplies libtiff 4.7.2. After pulling this increment, rerun
`conan install . -pr:h profiles/macos-debug -pr:b sipi-2-local --build=missing`
before configuring/building in CMake or CLion.

`src/imgformats/TiffMetadata.h` reads delivery metadata without pixel decoding.
`src/iiifparser/IIIFGeometry.h` resolves region/size against image dimensions and
policy. `--size-upscaling true`, `SIPI_SIZE_UPSCALING=true`, or TOML
`size-upscaling = true` enables actual bilinear enlargement (default false), with ^ requests and output limits.
See [upscaling](docs/upscaling.md).
The HTTP worker now resolves sources, reads TIFF metadata, and checks geometry.
Configure `--iiif-root`, `SIPI_IIIF_ROOT`, or TOML `iiif-root` for default identifier
mapping and relative Lua source paths. Absolute script overrides need no root.
See [source resolution and manual test images](docs/source-resolution.md).
See [the contract, examples, and feature roadmap](docs/tiff-metadata-and-geometry.md).

The first pixel reader is now available in `src/imgformats/TiffReader.h`:
`read_tiff_region` reads only intersecting tiles or strips from top-left, contiguous gray/RGB/RGBA8
TIFFs into a format-neutral `RasterImage8`. It preserves ICC bytes and rejects other
layouts explicitly. `tests/TiffRegionTests.cpp` verifies exact pixels, selective
access, and error paths. HTTP preflight selects local or S3 sources; TIFF crop
reading, [area-weighted reduction](docs/area-resize.md) and JPEG/PNG output are integrated.

## Lua preflight

The independent `preflight` target loads a trusted Lua script and evaluates
`preflight(request)` into a typed allow/deny decision with an optional source path.
Each call owns a fresh Lua state; script failures and invalid results are errors,
not access grants. See [the contract and C++ example](docs/preflight.md) and
[the sample script](examples/preflight.lua). Generic [Lua JSON helpers](docs/lua-json.md) support nested data, null and explicit array/object identity.
[Lua HTTP(S) requests](docs/lua-http.md) provide bounded external API access with verified TLS and explicit headers.
[Lua JWT helpers](docs/lua-jwt.md) sign and verify HS256/RS256 tokens with explicit keys and claim policy.
[Shared Lua value caching](docs/lua-cache.md) provides bounded TTL storage and coalesced loading across requests.
A tested [OLDAP policy example](docs/oldap-preflight.md) combines media-token authorization, optional anonymous REST lookup and repository mapping.
[Script configuration/secrets](docs/lua-configuration.md) populate `sipi.config` and `sipi.secrets`
with per-name CLI/environment/TOML precedence and optional startup secret-file snapshots. IIIF image/info GET and HEAD requests invoke it on a dedicated bounded worker. Configure the script with
`--preflight-script examples/preflight.lua`, `SIPI_PREFLIGHT_SCRIPT`, or the TOML
key `preflight-script`. Startup validates the file and entry point before listening;
invalid configured scripts fail startup. Relative paths use the working directory.
No script is loaded by default. Denials return 403, script failures 500, and
missing selected files 404; invalid geometry returns 400. Supported tiled or strip-based gray/RGB/RGBA8 TIFF crops now return JPEG or PNG; unsupported layouts and
transformations return 501, except unsupported rotation angles (400).
Horizontal mirroring and 90° steps are supported; see
[transformation behavior and examples](docs/orthogonal-transforms.md). Information requests now
return 200 with real dimensions and public service identity. See [HTTP behavior](docs/http-server.md).

After this dependency change, rerun the Conan install and CMake configure/build
commands above. In CLion, reload the `conan-debug` CMake preset; the new
`preflight-tests` target is runnable directly or through CTest (`lua-preflight`).

## Image information

After building and running CTest (which generates the TIFF fixtures):

```sh
./build/Debug/sipi-2 --iiif-root build/Debug/http-test-data
curl http://127.0.0.1:8080/project/sample.tif/info.json
```

The result describes the authorized source (320 x 200 here), preserving `project`
in the public service id. Configure `public-base-url = "https://images.example.org"`
behind Caddy. CLI and SIPI_PUBLIC_BASE_URL overrides work as for other settings.

Admitted resources publish `profile: level2`. Base URIs redirect after authorization;
info supports JSON/JSON-LD negotiation. CORS defaults to public wildcard access,
with configurable explicit origins and cookie support. See [HTTP policy](docs/iiif-http.md).
External-validator and deployed-browser checks remain outstanding.
See [the info.json contract](docs/iiif-info.md). Image responses include a
[canonical URL](docs/canonical-uri.md) in the Link header, alongside the profile
link, while delivering pixels directly (also on cache hits and HEAD).

## First JPEG delivery

The generated `tiled.tif` fixture supports real image output:

```sh
./build/Debug/sipi-2 --iiif-root build/Debug/http-test-data
```

Open `/tiled.tif/full/max/0/default.jpg` for the full image or
`/tiled.tif/50,30,100,80/max/0/default.jpg` for a crop. The earlier `sample.tif`
is strip-based and also supports pixel delivery.

The independent JPEG writer uses libjpeg 9f (now a direct Conan dependency),
preserves ICC profiles, and emits synchronous byte blocks. HTTP currently buffers
up to 64 MiB before responding. See [the supported subset, limits and tests](docs/jpeg-delivery.md).

## Optional local S3 test storage

See [MinIO development setup](docs/minio-development.md) for Docker Compose,
persistent test storage and local endpoints. The [S3 byte-source adapter](docs/s3-byte-source.md)
is integrated with HTTP and Lua source selection. Try
`./build/Debug/sipi-2 --config examples/minio.toml` with the local test bucket.
The IIIF URL prefix is excluded from default storage mapping; project-specific
translation belongs in preflight. See [repository configuration](docs/command-line.md#repository-selection-local-or-s3).
MinIO integration tests are opt-in with `-DSIPI_TEST_S3=ON`.

S3 credentials support `--s3-access-key`, `--s3-secret-key` and
`--s3-session-token`, their `SIPI_S3_*` environment equivalents and matching TOML
keys, with the usual precedence. See [credential configuration](docs/command-line.md#s3-credentials).


A [pure TIFF read planner](docs/tiff-metadata-and-geometry.md#pure-tiff-read-planning)
now selects suitable pyramid levels and preserves exact fractional crop edges.
Delivery uses the selected IFD and exact fractional-window resampling, locally and over S3.


## PNG output

RGB8 PNG output now shares TIFF pyramid selection and exact crop/reduction with
JPEG. Use `.png`, for example `/tiled.tif/50,30,100,80/37,29/0/default.png`.
See [PNG delivery](docs/png-delivery.md) for ICC handling, limits and tests.
Conan now includes libpng 1.6.58. Rerun the normal Conan install command and reload
CMake in CLion after pulling. RGBA transparency is now supported; see [alpha delivery](docs/alpha-delivery.md).


## Transparency

Explicit straight and premultiplied TIFF alpha are supported for tiled or strip-based RGBA8
sources, including compatible pyramid levels. PNG retains transparency; JPEG
composites onto white. For generated fixtures, try
`/alpha.tif/full/max/0/default.png` and the same URL ending in `.jpg`.
Resizing weights colors by alpha to avoid hidden-color fringes. Color management
and arbitrary rotation remain separate steps.

Strip-based TIFF and native 8-bit grayscale now share the delivery pipeline,
including MINISBLACK/MINISWHITE normalization, pyramid selection and local/S3
access. PNG and JPEG retain a single gray channel and its ICC profile. See
[strips and grayscale](docs/tiff-strips-and-gray.md) for examples and limits.

JPEG 2000 input via Grok now supports headers and region/resolution pixel decoding locally and over S3. See
[JPEG 2000 input](docs/jpeg2000-input.md) for the local Conan recipe, CLion setup,
supported pixel layouts, read planning and resource limits.

The first cache increment defines stable encoded-result identities and SHA-256
keys, tested independently of HTTP. Persistent storage is now implemented below;
see [cache identity](docs/cache-key.md) for normalization and policy boundaries.


Persistent encoded-result storage now uses files and a fixed `cache.sqlite3`
index. Configure `cache-dir` / `SIPI_CACHE_DIR` / `--cache-dir`; precedence remains
CLI > environment > TOML > defaults. Without a directory it stays disabled.
The default encoded-file limit is 1 GiB, with LRU cleanup to 90%; both are
configurable. Startup recovery and exclusive directory locking are implemented.
JPEG/PNG image responses now use authorized cache lookup/fill, including local and
S3 sources. Hits skip pixel decoding, processing and encoding. A bounded in-memory
metadata cache also reuses unchanged source headers after version checks. Cache failures fall back to uncached delivery. See [persistent cache](docs/persistent-cache.md) and
[startup configuration](docs/command-line.md).


The [in-memory metadata cache](docs/metadata-cache.md) retains immutable TIFF/J2K
source facts, including ICC profiles and resolution information, for up to 256
entries / 64 MiB of accounted storage by default. Configure
`metadata-cache-max-entries` and `metadata-cache-max-bytes` via CLI/environment/TOML;
either zero disables retention. It is independent of `cache-dir` and empty
after restart. Preflight and local/S3 source-version checks still run on every
request; successful metadata and encoded-result hits avoid repeated image reads.

### Parallel IIIF processing

Image work runs in a bounded pool, defaulting to the detected logical CPU count
capped at four (fallback one). Configure `--iiif-workers`, `SIPI_IIIF_WORKERS`, or
TOML `iiif-workers` (1..32). The metadata cache is shared; persistent-cache fills
for the same result are coordinated. See [worker concurrency](docs/worker-pool.md)
for lifecycle, memory considerations and test coverage.


### Measuring worker concurrency

The opt-in [tile benchmark](docs/benchmarks.md) builds deterministic large TIFF/J2K
fixtures and compares 1/2/4 workers in Release with empty/populated result caches.
It records throughput, latency, sampled RSS and verifies every response against
serial output. An isolated Drogon/Trantor TSan build is documented there too.


### JPEG input

Accepted 8-bit gray/RGB JPEGs now support `/{identifier}/info.json` and HEAD,
locally and over S3, plus cropped/reduced JPEG and PNG output with mirroring and
quarter turns. Baseline/progressive pixels are fully decoded; ICC profiles are
retained. EXIF orientation must be absent or Top-left. See [JPEG input](docs/jpeg-input.md).


### PNG input

PNG sources now support information and JPEG/PNG pixel delivery locally and over
S3, with palette/low-bit expansion, transparency, Adam7 and ICC preservation.
The existing processing/cache pipeline is reused. Current exclusions include
16-bit, EXIF/APNG and gamma/chromaticity-only declarations; see
[PNG input](docs/png-input.md) for the complete color policy and limits.

Explicit PNG sRGB now survives processing and PNG/JPEG output without a pixel color
transform. See [color declarations and conversions](docs/color-management.md) for
the exact assignment/preservation rules and remaining limitations.

Arbitrary IIIF rotation angles now use bilinear interpolation with transparent
borders (white in JPEG). Exact quarter turns retain their fast paths. See
[arbitrary-angle rotation](docs/arbitrary-rotation.md) for geometry, color and limits.

IIIF `gray` now converts supported RGB/RGBA sources with LittleCMS to Gray Gamma
2.2/D50, preserving alpha and embedding the target ICC. Untagged RGB assumes sRGB
only for this conversion; native grayscale keeps its values/profile. See
[color handling](docs/color-management.md) for the complete policy and limitations.


IIIF `bitonal` now builds on that grayscale conversion: composite transparency on
white, then threshold at 128 without dithering. PNG contains exact black/white
samples in packed 1-bit grayscale storage; JPEG compression may introduce gray artifacts.
See [the bitonal policy](docs/color-management.md#bitonal-quality).


Optional `--bitonal-method=floyd-steinberg` enables serpentine error diffusion;
`SIPI_BITONAL_METHOD` and TOML `bitonal-method` use the usual precedence.
Threshold remains the default. See [color management](docs/color-management.md)
for the important limitation when rendering separate IIIF tiles.


Public output bounds are configured with --max-width, --max-height and --max-area
(or SIPI_MAX_WIDTH/HEIGHT/AREA and corresponding TOML keys). Defaults: 8192 per
axis, 8388608 pixels; absent height inherits width. The same limits appear in
info.json and are enforced before cache lookup. Unsupported or predictably
unaffordable sources fail admission consistently. See [output policy](docs/output-limits.md).

## WebP output

The IIIF delivery pipeline also supports .webp, announced in info.json extraFormats.
Color defaults to lossy quality 85; gray/bitonal are lossless. Alpha is preserved;
gray ICC is correctly converted to RGB sRGB rather than copied to the wrong color
model. Local/S3 sources, cache/restart, canonical links and HEAD are supported.
Run Conan install and reload CMake after updating: libwebp/1.6.0 is newly pinned.
See [WebP delivery and color policy](docs/webp-delivery.md).

## Docker trial

See [Docker build and local OLDAP trial](docs/docker.md) for the Linux image,
listener configuration and reversible Cantaloupe replacement.

## Single-color watermarks

A standalone [single-color watermark compositor](docs/watermarks.md) supports
centered coverage masks and full-image-anchored crops on native or reduced decoder grids,
sampled directly from the original mask without a canvas-sized intermediate.
A bounded PNG mask loader accepts untagged gray 1/2/4/8-bit PNGs, including Adam7.
A separate color converter maps the chosen sRGB color into the image encoding
without converting or retagging the image. Lua selects the local/S3 mask, relative
width, opacity and sRGB color. Delivery applies it before IIIF transforms and binds
all settings and the mask version into the result cache. A missing or invalid
required mask fails delivery even on cache hits; no unmarked fallback is served.

A bounded [decoded-mask memory cache](docs/watermarks.md#decoded-mask-memory-cache)
shares coverage samples across requests after current source-version checks.
Defaults: 64 entries / 32 MiB, configurable through CLI/environment/TOML.
