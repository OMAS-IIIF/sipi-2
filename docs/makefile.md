# Makefile: development, tests and image publication

Run targets from the repository root. `make` or `make help` lists commands.
The Makefile orchestrates existing CMake/Conan/Python/Docker tools; it does not
replace their configuration. Python helpers use only the standard library.

## Version changes

```sh
make show-version
make bump-patch-level   # 0.1.0 -> 0.1.1
make bump-minor-level   # 0.1.1 -> 0.2.0
make bump-major-level   # 0.2.0 -> 1.0.0
```

The sole authoritative version remains `project(sipi-2 VERSION ... LANGUAGES CXX)`
in CMakeLists.txt. `scripts/version.py` requires exactly one supported declaration,
changes only the version, preserves file permissions and atomically replaces the
file. Lower components reset for minor/major bumps. Commands do not commit, tag,
push Git, or publish images. Run bumps serially, without concurrent file edits.
Rebuild afterward so `--version` and `/health` adopt the changed CMake definition.

## Native build and tests

The initial Conan compiler profile must exist (see the [native build guide](building.md)).
For the documented macOS profile:

```sh
make dependencies
make build
make test
```

`dependencies` exports the checked-in Grok recipe and installs the lockfile's
packages using `HOST_PROFILE=profiles/macos-debug` and
`BUILD_PROFILE=sipi-2-local`. Override profiles on other development platforms;
no global profile is overwritten. The default build directory is `build/Debug`,
matching Conan's layout. `BUILD_DIR` can select an already configured tree,
including a sanitizer tree. A fresh custom tree needs its Conan toolchain first.
`JOBS=4` defaults to a modest compiler parallelism.

`make test` executes sequentially, stopping at the first failure:

1. Check Docker availability and the external validator checkout/Python environment.
2. Configure with `BUILD_TESTING=ON` and `SIPI_TEST_S3=ON`, build, start/wait for
   the existing development MinIO Compose project, and run **all registered CTests**.
   This includes configuration, codecs, Lua/auth, caches, HTTP, S3 and release
   helper tests. Fixture setup remains managed by CTest.
3. Run the unchanged external IIIF validator for original and restricted services,
   plus its companion WebP probes. Evidence goes to `build/external-validation.json`.
4. Build a native Docker runtime image and run both OLDAP and generic mounted-policy
   container checks. No registry push occurs.

Prerequisites are explicit; missing dependencies are errors, not skipped successes.
Install the external validator once following [external validation](external-validation.md).
MinIO remains running afterward for development. Its existing volume is preserved;
tests own disposable buckets. Container checks remove only their own uniquely named
containers/cache volumes. Images and build caches remain available for reuse.

Useful subsets:

```sh
make test-core                 # All CTests, including MinIO
make test-external             # External validator only, after building
make docker-local              # Native image + container checks, no push
make docker-test               # Both architectures + container checks, no push
make test-core BUILD_DIR=build/sanitize  # Reuse an existing sanitizer configuration
```

`make test` runs all test cases, not every possible compiler/instrumentation matrix.
ASan/UBSan use a separately configured build tree; full two-architecture container
verification is the additional publication gate in `docker-build`. Benchmarks are
measurements, not pass/fail tests, and are not part of this command.

## Docker build and publication

```sh
docker login
make docker-build
```

The default repository is `lrosenth/sipi-2`. The helper reads the same CMake version
and publishes **both** `lrosenth/sipi-2:VERSION` and `lrosenth/sipi-2:latest`, each
containing `linux/amd64` and `linux/arm64`. Publishing updates these registry tags;
it does not create a Git release. Use a new version for a new published release.

`scripts/docker_image.py` first builds/loads each platform separately and runs the
container checks with that explicit platform. A failed build/check stops publication.
Only after both pass does a multi-platform Buildx build push the version and latest
manifests, reusing cached build results. Publication requires a committed, clean Git
worktree, including staged, unstaged and untracked files (ignored build outputs do
not block it). The helper builds all publication stages from one fixed `git archive`
of that revision. It checks source revision/state before builds and after smoke
checks and stops if they changed. Keep release commands and source edits serial.
Docker's existing selected builder/login are used; the helper neither stores
credentials nor installs privileged emulation support.

Local verification tags are `lrosenth/sipi-2:verify-VERSION-amd64` and `...-arm64`.
The selected builder/engine must support both target platforms; Docker Desktop's
emulation can make the nonnative compiler build slow. A build failure never triggers
a single-architecture fallback. Both platforms are mandatory for publication.

```sh
make docker-build DOCKER_IMAGE=another-namespace/sipi-2 JOBS=4
make docker-test PLATFORMS=linux/arm64,linux/amd64
```

See [Docker runtime configuration](docker.md) for mounting media, Lua and TOML.
Buildx behavior follows Docker's [multi-platform documentation](https://docs.docker.com/build/building/multi-platform/)
and [build command reference](https://docs.docker.com/reference/cli/docker/buildx/build/).

## Image identity and release evidence

The helper supplies and verifies these labels on each loaded image:

| Label | Value |
| --- | --- |
| `org.opencontainers.image.title` | `SIPI-2` |
| `org.opencontainers.image.source` | `https://github.com/OMAS-IIIF/sipi-2` |
| `org.opencontainers.image.version` | Version read from CMake; checked by the runtime smoke test |
| `org.opencontainers.image.revision` | Full Git commit ID; `-dirty` suffix for uncommitted local/test builds |
| `org.omas-iiif.sipi.source-state` | `clean` or `dirty` |

Raw `docker build` invocations receive `unknown` version/revision/state defaults,
rather than the inherited Ubuntu version. Use the helper for verified metadata.
Local/test actions continue to support development edits. A dirty label records a
base commit plus pending changes; it is **not** a hash of those changes or proof of
reproducibility. A clean label alone is not a signature or supply-chain attestation.
The canonical source URL identifies this project; fork maintainers should adapt it.

Each invocation prints a unique report directory under `build/releases/`. It contains
`report.json` with source state, version, per-platform image IDs/result digests and
smoke outcomes, plus full Buildx metadata JSON. The publish path also records the
pushed result digest from `published.json`. Reports are local/ignored: retain the
successful report with release artifacts deliberately; do not commit machine-local
Buildx metadata blindly. Image IDs, platform manifests and multi-platform index
digests are different identities, so use the recorded published digest for a
published multi-platform reference.

```sh
docker image inspect lrosenth/sipi-2:verify-0.1.3-arm64 \
  --format '{{json .Config.Labels}}'
```

A failed/interrupted invocation records failure rather than success. If a registry
push was attempted, the report marks that uncertainty until the returned digest is
validated: inspect the registry before retrying after an interrupted push. Registry
tag updates are not a transaction across tags. This workflow does not create or
push Git tags, create a GitHub release, prevent overwriting an existing version tag,
or retroactively prove where older published images came from. Choose a new release
version and an intentional Git tag/release policy before publishing.

The final multi-platform build still uses BuildKit's cached results from the
verified platforms; its actual returned digest is recorded separately. Mutable
base-image/package repositories and cache behavior mean this is not a promise of
bit-identical independent rebuilds. See Docker's
[build metadata reference](https://docs.docker.com/reference/cli/docker/buildx/build/#write-build-result-metadata-to-a-file---metadata-file).
