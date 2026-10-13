# Source release package

## Create a package

Requirements: Python 3.12 or newer, Git, Docker with Buildx and Linux AMD64/ARM64
support (native builders or emulation). Run from a **clean committed checkout**:

```sh
make source-package
```

CMake supplies the version. The target exports sources from both actual Linux
build graphs using a fixed `git archive` of HEAD. It may download missing sources
and build images, but never pushes images, creates tags or publishes a GitHub
release. Existing outputs are never overwritten. No ignored/untracked local
files enter the project snapshot, including excluded test photographs.

Output is under `build/source-releases/<version>-<commit-prefix>/`:

- `sipi-2-<version>-source.tar.gz`: project and dependency source package.
- `SHA256SUMS`: checksum of that archive.
- `manifest.json`: commit, version, platform graphs and payload checksums.

Override the destination with `SOURCE_OUTPUT=/absolute/path`; use a directory
outside the checkout or an ignored build directory. `JOBS=4` controls build
concurrency. A first run can take substantial time and disk space because
Conan must retrieve source even for libraries originally installed as binaries.
BuildKit caches are reused. Temporary packaging output is removed on failure;
Conan/BuildKit caches are retained. Old successful release directories remain.

For the final release, bind the package to a successful clean two-platform
`make docker-test` or `make docker-build` report:

```sh
make source-package SOURCE_IMAGE_REPORT=/absolute/path/to/report.json
```

This compares both runtime dependency inventories (including exact recipe and
binary-package revisions) using the immutable local image IDs in that report.
The images must still be available locally. Without that option the manifest
explicitly records `image_report_checked: false`: the archive belongs to its
source-export builds, but has not been matched to the final verified images.
A later published multi-platform digest must also be recorded in the release
notes; a successful pre-publication test is not proof of a later registry push.

## Contents after extraction

```text
sipi-2-<version>-source/
  README.md
  manifest.json
  sipi-2/                         # committed project, docs, licenses and recipes
  dependencies/
    amd64/
      conan-sources.tgz          # Conan recipes, exported patches and sources
      recipes.json              # exact recipe revisions; no binary package IDs
      dependencies.json         # graph settings/options and source coverage
      conan.lock                # resolved Linux lockfile
      licenses/                 # runtime dependency notices and inventory
    arm64/                      # same structure for the other architecture
```

The Conan archives are ordinary tar/gzip files in Conan's restore format. They
contain no prebuilt Conan binary packages. All host library sources are required,
including Grok's downloaded embedded components. Build-tool recipes are included;
build-tool sources are included only when available. General-purpose build tools,
Python/Conan, compilers and the Ubuntu operating system are prerequisites rather
than part of this source package. Ubuntu's separate distribution/source obligations
must still be considered when distributing the complete container image.

## Verify and restore sources

Check the outer archive before extraction:

```sh
shasum -a 256 -c SHA256SUMS
```

Use Conan **2.33.0**, the version pinned in the Dockerfile, for the cache format.
From the extracted package root, select `arm64` or `amd64` to match the Linux host:

```sh
export CONAN_HOME="$PWD/.conan-source-build"
conan cache restore dependencies/arm64/conan-sources.tgz
conan profile detect
```

This creates a separate Conan home and does not overwrite an existing development
cache. Inspect any library with `conan cache path 'name/version#revision' --folder
source`, using the exact reference from `dependencies.json`. `conan cache path`
without `--folder` locates its recipe. Exported patches are accessible with
`--folder export_source`. Do not edit the restored cache; copy source to a working
directory for development and follow the library's own build instructions.

## Build with the restored recipes and sources

Use the Linux compiler/tool environment documented in `sipi-2/Dockerfile` (Ubuntu
24.04, GCC, CMake, Ninja, pkg-config, Autotools, NASM, Python and Conan). Once the
previous restore commands have completed, an ARM64 example is:

```sh
cd sipi-2
conan install . -pr:h docker/conan-profile -pr:b docker/conan-profile \
  --lockfile=../dependencies/arm64/conan.lock --build='*' \
  -s build_type=Release -s compiler.cppstd=20 -c tools.build:jobs=4
cmake -S . -B build/Release \
  -DCMAKE_TOOLCHAIN_FILE=build/Release/generators/conan_toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/Release --parallel 4
build/Release/sipi-2 --version
```

Use `dependencies/amd64/conan.lock` on AMD64. The restored archive already includes
the exported Grok recipe. `--build='*'` forces compilation instead of substituting
remote library binaries. General-purpose build tools may still be downloaded;
this workflow does not promise a fully offline or bit-identical rebuild. A full
fresh rebuild is separate from validating archive contents and cache restoration.
The regular native build guide in `sipi-2/docs/building.md` also applies, but other
platforms can need dependencies not present in the Linux graphs.

## Publish on GitHub

Attach the archive, `SHA256SUMS` and `manifest.json` to the GitHub release for the
same source commit/tag. Make them publicly downloadable and link them from the
Docker image description beside the matching version/digest. Retain these files
for as long as required by the applicable licenses. GitHub's automatically
created repository tarball is useful but does not replace this dependency bundle.
Before publication, verify the final image/source association, license notices
and source coverage; a file inventory is not a legal certification.
