# Container build and local OLDAP trial

Build from the sipi-2 repository root:

```sh
docker build -t sipi-2:local-test .
docker run --rm sipi-2:local-test --version
python3 tests/docker_smoke.py sipi-2:local-test
```

The multi-stage Ubuntu 24.04 build uses Conan 2.33.0, C++20 and the checked-in
lockfile, including Linux-only libuuid/s2n dependencies. The local Grok recipe is
exported before dependency resolution. A Docker-specific derived lock preserves
those library pins while selecting the exact Autotools versions supplied by Ubuntu
through Conan's platform_tool_requires profile (GNU mirror downloads are avoided). BuildKit retains the Conan cache; the first
build can take substantially longer than later source-only builds. BUILD_JOBS
(default 4) limits parallel compilation. Builds target the Docker engine's native
architecture; cross-platform images require a separate validation run.

The runtime runs as UID/GID 10001, includes CA certificates and the required
Conan shared libraries, and has no build toolchain. Images mount at `/data/images`;
`/var/cache/sipi` is pre-owned for a separate cache volume. The generic image does
not enable an image root, cache, script or credentials automatically. Its only
configuration default override is `SIPI_LISTEN_ADDRESS=0.0.0.0`; port remains 8080.
Supply settings using the normal environment/CLI/TOML interfaces.

Example public-only local fixture server (no access policy):

```sh
docker run --rm -p 127.0.0.1:8080:8080 \
  --mount type=bind,src=/absolute/path/to/test-images,dst=/data/images,readonly \
  sipi-2:local-test --iiif-root=/data/images
```

For a read-only root filesystem, use a writable scratch working directory such as
`/tmp` on tmpfs for Drogon's runtime directories. Repository data should stay
read-only; persistent rendered output belongs in a separate cache volume.

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
