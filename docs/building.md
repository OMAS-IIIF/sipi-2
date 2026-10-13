# Native build and development

Run commands from the repository root. Docker is not required to build or run
the native server. Docker/MinIO are needed for the separate S3/container integration
suites described below. For an immediate container-based example, use the
[root quickstart](../README.md#quickstart-with-docker).

## Prerequisites and platform scope

- A C++ compiler supporting C++20 for SIPI-2 and C++23 for Grok.
- CMake 3.24 or newer, Ninja and Conan 2.
- Python 3 for workflow scripts and HTTP integration tests; these project scripts
  use the standard library.
- Git and the build tools required by dependencies compiled from source. The
  [Dockerfile](../Dockerfile) records the Linux environment, including pkg-config,
  Autotools and NASM. It pins Conan to 2.33.0 in that environment.

The checked-in native profile targets local macOS development with Apple Clang.
Linux AMD64/ARM64 container builds use [a separate profile](../docker/conan-profile).
Do not equate container validation with testing every native Linux distribution
or compiler. A generic Windows build is not documented as validated.

Dependencies are declared in [conanfile.txt](../conanfile.txt); the lockfile pins
resolved versions and recipe revisions, not your compiler or OS SDK. Conan supplies
Lua and the image/network libraries; a separate system Lua installation is not
needed. Grok uses the checked-in [recipe](../conan/recipes/grok/conanfile.py), which
must be exported before dependency installation.

## macOS Debug build

Detect a compiler profile once on a new machine:

```sh
conan profile detect --name sipi-2-local
conan profile show -pr:h profiles/macos-debug -pr:b sipi-2-local
```

Inspect the compiler, architecture and SDK settings. The detected profile lives
in your Conan user cache; it is machine-specific and is not committed. If that
profile already exists, inspect it instead of overwriting it blindly.

Install dependencies, configure and build:

```sh
conan export conan/recipes/grok
conan install . -pr:h profiles/macos-debug -pr:b sipi-2-local --build=missing
cmake --preset conan-debug
cmake --build --preset conan-debug --parallel 4
```

Conan reads `conan.lock` automatically and generates CMake dependency metadata,
the toolchain, `CMakeUserPresets.json` and the `conan-debug` preset.
`-pr:h` selects the target environment; `-pr:b` selects build-tool settings.
`--build=missing` compiles packages unavailable as compatible binaries.

[profiles/macos-debug](../profiles/macos-debug) includes `sipi-2-local`, selects
Debug/C++20/Ninja, and scopes an Apple Clang unused-result warning exception to
Trantor. The compiler must still support C++23 for Grok's own build.
Other platforms need matching profiles; do not copy machine-specific compiler
settings from a different computer.

For ordinary source edits, repeat only the CMake build command. After dependency
or build-setting changes, repeat Conan install and configure. If a build tree was
configured without the current Conan toolchain, use
`cmake --fresh --preset conan-debug` to discard stale CMake configuration.

## Release build

The same macOS profile can supply compiler settings with an explicit Release
override. Keep Debug and Release in separate generated directories:

```sh
conan install . -pr:h profiles/macos-debug -pr:b sipi-2-local \
  -s:h build_type=Release --build=missing
cmake --preset conan-release
cmake --build --preset conan-release --parallel 4
```

The executable is `build/Release/sipi-2`. Regenerate the Debug preset with its
Conan install command when returning to that configuration if needed. The
Makefile's initial configuration defaults to Debug; use the explicit Release
sequence above rather than assuming `BUILD_DIR` alone selects a build type.

## First image without Docker

The repository includes a [synthetic public example](../examples/images/README.md)
independent of the optional private benchmark corpus:

```sh
./build/Debug/sipi-2 --version
./build/Debug/sipi-2 --port 18080 --iiif-root examples/images
```

In a second terminal:

```sh
curl --fail http://127.0.0.1:18080/health
curl --fail http://127.0.0.1:18080/sample.png/info.json
curl --fail http://127.0.0.1:18080/sample.png/full/160,/0/default.jpg -o preview.jpg
```

The source is 320 × 200; the preview is 160 × 100. Use `build/Release/sipi-2`
for a Release build. The listener defaults to loopback. Stop the server with
Ctrl-C or SIGTERM. For a public service behind a proxy, set `public-base-url` to
the external origin and mount path; configure TLS at the proxy.

Paths such as `iiif-root` and `preflight-script` are relative to the process's
working directory, not the configuration file. See the
[configuration reference](command-line.md) and [source mapping](source-resolution.md).
The source-tree binary can depend on Conan-built shared libraries, including Grok;
copying only the executable to another machine is not a complete deployment.

## Tests

For the currently configured native test suite:

```sh
cmake --build --preset conan-debug --parallel 4
ctest --preset conan-debug --output-on-failure
```

CMake's `BUILD_TESTING` controls test registration. `SIPI_TEST_S3=ON` adds MinIO
integration tests; these require the documented [MinIO setup](minio-development.md).
Tests generate their codec/HTTP fixtures; the two private originals in
`test-images/` are optional benchmark inputs, not a prerequisite for the ordinary
native test suite. Tests cover configuration, parsing, image processing, codecs,
Lua policies, caches and HTTP behavior. HTTP tests manage temporary servers.

For the broader project verification workflow:

```sh
make test
```

This requires Docker and a separately prepared
[external validator](external-validation.md). It enables S3 tests, starts the
existing development MinIO service, runs all registered CTests, then external
IIIF validation and native container checks. It does not publish an image.
Read the [Makefile guide](makefile.md) for prerequisites, test subsets, persistent
MinIO data and multi-architecture release checks.

## CLion

1. Install dependencies with Conan, then open this repository as a CMake project.
2. Select your local C++ compiler and debugger. On macOS this is normally Apple
   Clang and LLDB. Select an installed CMake meeting the minimum version.
3. Reload CMake presets and enable the generated `conan-debug` profile. Disable
   older manual profiles that omit the Conan toolchain.
4. Select the `sipi-2` executable configuration. Set the repository as the working
   directory and add `--port 18080 --iiif-root examples/images` to run the example.

No Conan IDE plugin is required. Regenerate/reload presets after dependency changes.
Generated user presets, `.idea/` and build directories are deliberately ignored.

## Code organization

The [delivery architecture](iiif-delivery-architecture.md) describes request flow
and ownership boundaries. `src/main.cpp` owns startup and `src/CommandLine.cpp`
resolves settings; HTTP integration lives in `src/HttpServer.cpp` and `src/http/`.
Image processing, codecs, byte sources, caches, IIIF parsing and Lua policies have
separate CMake targets. Libraries use explicit value types and RAII; prefer owning
standard-library containers and keep borrowed views within their owner's lifetime.

For maintained context use [codex.md](../codex.md); the chronological technical
work log is [CODEX_LOG.md](../CODEX_LOG.md). Neither replaces the public guides.
