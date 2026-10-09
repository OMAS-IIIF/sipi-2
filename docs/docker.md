# Container build and local OLDAP trial

## Build and publish

From the repository root:

```sh
make docker-local  # Native architecture, real runtime tests, no publication
make docker-test   # AMD64 and ARM64, tests for both, no publication
make docker-build  # Tests both, then pushes lrosenth/sipi-2:VERSION and :latest
```

CMake's version is the single tag source. `DOCKER_IMAGE` and `JOBS` are configurable.
See the [Makefile workflow](makefile.md) for prerequisites, test scope and exact
publication behavior. The ordinary `docker build -t sipi-2:local-test .` and
`python3 tests/docker_smoke.py sipi-2:local-test` commands also remain supported.

## Runtime image contents

The build stage uses Ubuntu 24.04, Conan 2.33.0, C++20 and the lockfile with the
local Grok recipe. Its derived Linux lock preserves library pins while selecting
Ubuntu's installed Autotools through the Docker Conan profile. BuildKit's Conan
cache is locked while each stage uses it, preventing cross-architecture writes
from racing. `BUILD_JOBS` defaults to four.

The final stage starts separately from Ubuntu 24.04 and contains only the stripped
SIPI executable, required runtime/shared libraries, CA certificates and the small
optional OLDAP Lua/entrypoint example. It contains **no SIPI C++ sources, Conan
cache, static libraries, compiler, CMake, Ninja or Python build environment**.
Stripping removes unused symbols from the copied executable/libraries; build-stage
symbols remain available locally for debugging. Build layers may be large but are
not distributed as runtime layers. Runtime content and both mounted-policy paths
are checked by `tests/docker_smoke.py`.

The process runs as UID/GID 10001. Media and scripts must be readable by this user.
`/var/cache/sipi` is pre-owned for a Docker named cache volume. The image enables
listening on `0.0.0.0` but does not select a repository, credentials or preflight
policy. All are runtime choices. HTTPS remains the reverse proxy's responsibility.

### Verified build (2026-10-08)

`make docker-test` built and tested both linux/amd64 and linux/arm64 successfully,
including version/health, OLDAP token handling and generic mounted media/Lua/cache
configuration. Runtime inspection verifies the absence of source/build tools as
root; application tests run as the image's nonroot user. The generic Compose
example also served a TIFF as PNG with its file-mounted Lua policy. No registry
publication was performed during this verification. The first emulated Intel build
had to compile Grok, Lua, Drogon and the AWS SDK; those are now in the build cache.

## Mount media and an arbitrary Lua policy

No rebuild is needed to choose a repository or script. For an already built ARM64
verification image (choose the appropriate architecture/version tag):

```sh
docker run --rm --read-only --tmpfs /tmp --workdir /tmp \
  --cap-drop=ALL --security-opt=no-new-privileges:true \
  -p 127.0.0.1:8080:8080 \
  --mount type=bind,src=/absolute/path/to/images,dst=/data/images,readonly \
  --mount type=bind,src=/absolute/path/to/preflight.lua,dst=/etc/sipi/preflight.lua,readonly \
  --mount type=volume,src=sipi-cache,dst=/var/cache/sipi \
  -e SIPI_IIIF_ROOT=/data/images \
  -e SIPI_PREFLIGHT_SCRIPT=/etc/sipi/preflight.lua \
  -e SIPI_CACHE_DIR=/var/cache/sipi \
  -e SIPI_PUBLIC_BASE_URL=http://localhost:8080 \
  lrosenth/sipi-2:verify-0.1.0-arm64
```

The public URL must match the externally visible origin/mount. Lua source paths
refer to paths **inside** the container: relative selections use `/data/images`.
Scripts are snapshotted at startup; restart/recreate the container after a change.
If a host bind-mounted single script is atomically replaced, recreate the container
to adopt the new file. Mount a configuration directory for deployments that replace
configuration files atomically. The image's entrypoint is SIPI itself; the packaged
OLDAP adapter is optional, not a required part of generic delivery.

Equivalent CLI options can follow the image name:

```sh
# Replace the -e settings above with:
# IMAGE --iiif-root=/data/images --preflight-script=/etc/sipi/preflight.lua \
#       --cache-dir=/var/cache/sipi --public-base-url=http://localhost:8080
```

To use TOML, mount a read-only configuration directory at `/etc/sipi` and append
`--config /etc/sipi/sipi.toml` after the image name. For example:

```toml
iiif-root = "/data/images"
preflight-script = "/etc/sipi/preflight.lua"
cache-dir = "/var/cache/sipi"
public-base-url = "http://localhost:8080"
port = 8080
```

The same CLI > environment > explicit TOML > default precedence applies. Use
`SIPI_PORT` or `--port` for another internal port and adjust Docker's mapping too.
The media mount stays read-only; only the cache volume and `/tmp` need writes.
A host-directory cache bind mount must already be writable by UID/GID 10001.

## Compose example

`docker/compose.example.yaml` supplies read-only media/Lua mounts, a persistent
cache volume, a read-only root filesystem and loopback-bound publishing. It is
independent of OLDAP and does not replace its deployment configuration.

```sh
export SIPI_IMAGE=lrosenth/sipi-2:verify-0.1.0-arm64
export SIPI_MEDIA_DIRECTORY=/absolute/path/to/images
export SIPI_LUA_SCRIPT=/absolute/path/to/preflight.lua
export SIPI_PUBLIC_BASE_URL=http://localhost:8080
docker compose -f docker/compose.example.yaml up -d
```

For deployment, select a published version tag after publication. Missing host
paths fail instead of being silently created. `SIPI_HOST_PORT` changes the exposed
host port; remember to update the public URL. A named cache volume survives ordinary
container recreation. `docker compose down` keeps it; `down -v` removes it.

## OLDAP integration boundary

The sibling oldap-mediaserver repository provides `docker-compose.sipi-test.yml`.
Use it together with its existing compose file; do not replace the base file.
The override reuses service name `imageserver`, container name and port 8182, so
Caddy continues to proxy `/iiif/*` without changes. sipi-2 receives `/iiif/3/...`
as its IIIF prefix. Therefore SIPI_TEST_PUBLIC_ORIGIN must be only the public
origin, for example `http://localhost:8088`, **without** `/iiif/3`; otherwise the
information document and canonical URLs would duplicate that prefix.

The shipped `examples/oldap/docker-entrypoint.sh` translates the existing dedicated
OLDAP media key, issuer and audience to the Lua configuration namespace. Explicit
SIPI settings take precedence; credentials never appear in command arguments.
The script explicitly sets minimum_permission=1 to preserve the existing image
rule. Set SIPI_PREFLIGHT_CONFIG_MINIMUM_PERMISSION=2 for full-view-only delivery.
No access/refresh key is used. The image repository is read-only and original
images are never converted or modified by this trial.

Anonymous lookup is disabled by default. Set OLDAP_SIPI_ANONYMOUS=true and provide
OLDAP_API_URL to enable it. The reserved OLDAP `unknown` pseudo-user needs no
password; an empty legacy OLDAP_UNKNOWN_PASSWORD is not exported as a SIPI secret.
A different configured service account requires a nonempty password. Missing
media tokens use the anonymous permission lookup; invalid supplied tokens remain
denied and never trigger anonymous fallback. See
[oldap-preflight.md](oldap-preflight.md) for the exact migration contract. Container
startup adopts environment/script snapshots; changing them requires recreation.

Follow the sibling repository's `docs/sipi-test.md` for activation and rollback.
This is a local trial image, not a published or production deployment.
