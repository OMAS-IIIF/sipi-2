# Running SIPI-2 with Docker

Use the published image to run SIPI-2 without installing compilers, Conan or Lua
on the host. The examples below use **`lrosenth/sipi-2:0.1.3`**, available for
Linux AMD64 and ARM64. Docker selects the matching architecture automatically.
Generic image delivery does not require OLDAP.

Commands assume a POSIX shell, Docker with Compose v2, and a checkout of this
repository. Run them from the repository root; use a second terminal for `curl`.
The examples reuse port 18080, so stop one example before starting the next.
For native installation instead, see [building.md](building.md).

## Contents

- [First image](#first-image)
- [Mount points and permissions](#mount-points-and-permissions)
- [Container configuration](#container-configuration)
- [TOML, Lua and a persistent cache](#toml-lua-and-a-persistent-cache)
- [Compose example](#compose-example)
- [S3 and secrets](#s3-and-secrets)
- [Reverse proxy and public URLs](#reverse-proxy-and-public-urls)
- [Logs, updates and troubleshooting](#logs-updates-and-troubleshooting)
- [Building your own image](#building-your-own-image)
- [OLDAP integration boundary](#oldap-integration-boundary)

## First image

If you do not already have the repository:

```sh
git clone https://github.com/OMAS-IIIF/sipi-2.git
cd sipi-2
```

Pull and inspect the executable version:

```sh
docker pull lrosenth/sipi-2:0.1.3
docker run --rm lrosenth/sipi-2:0.1.3 --version
```

Run with the [synthetic example image](../examples/images/README.md):

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

`info.json` describes 320 × 200 pixels; the JPEG preview is 160 × 100. Ctrl-C stops
the foreground server and `--rm` removes its container. This example enables
neither Lua nor a persistent cache: files resolvable inside the mounted repository
are accessible. To use your own images, change only the mount's host `src` and use
an identifier matching a supported file. Input restrictions are in the
[feature catalog](features.md#input-formats).

## Mount points and permissions

The runtime runs as **UID/GID 10001**. Bind-mount source paths must exist on the
Docker host. `--mount type=bind` fails for missing paths instead of creating an
accidental directory. With Docker Desktop, the host directory must be shared with
Docker; with a remote Docker daemon, paths refer to that remote host.

| Container path | Contents | Access and persistence |
| --- | --- | --- |
| `/data/images` | Your image repository; also relative mask sources if you use watermarks | Usually a read-only bind mount; files readable and parent directories traversable by UID/GID 10001 |
| `/etc/sipi` | Optional `sipi.toml`, Lua policy and other operator configuration | Read-only directory bind mount; select TOML explicitly with `--config` |
| `/etc/sipi/preflight.lua` | Alternative single-file Lua mount used by the Compose example | Read-only; host source must be a regular file |
| `/var/cache/sipi` | Encoded results, SQLite index and cache lock | Writable named volume, or a pre-existing writable host directory; dedicated to one server instance |
| `/run/secrets/...` | Optional operator-supplied secret files | Read-only mounts; each secret file must be readable by UID 10001 |
| `/tmp` | Temporary/runtime working files in the read-only-root examples | Writable tmpfs; not persistent |

These are conventions used by the examples, not mandatory paths baked into source
mapping. Use the configured path **inside the container**, never the host source
path, in `iiif-root`, Lua results and secret-file options.

A new Docker named volume at `/var/cache/sipi` inherits the directory ownership
provided by the image. An existing volume or host cache bind mount retains its
existing permissions. For bind mounts, provision that dedicated directory with
ownership or ACLs allowing UID/GID 10001 to write. Media/configuration only need
read/traverse access; do not recursively change ownership of an image collection
just to enable the demo. Do not mount the same persistent cache directory into
multiple running SIPI-2 instances: it is locked for single-instance use.

The image normally works in `/home/sipi`. For a read-only root filesystem, the
examples use `--tmpfs /tmp --workdir /tmp`, since Drogon may create an `uploads/`
working directory. This does not expose an image-upload API. Only `/tmp` and the
configured cache need writes for these examples.

## Container configuration

SIPI uses the same **CLI > environment > explicit TOML > default** order in Docker
and natively. The entrypoint is `/opt/sipi/sipi-2`: arguments after the image name
are SIPI arguments. Do not repeat the executable name there.

| Setting | Container-specific meaning |
| --- | --- |
| `SIPI_LISTEN_ADDRESS` | The image sets `0.0.0.0`. This image environment value overrides a TOML `listen-address`; an explicit CLI option can override it in turn. Listening only on container loopback normally prevents access through a published port. |
| `SIPI_PORT` / `--port` | Internal server port; default 8080. Docker `-p HOST:CONTAINER` must point to that port. `EXPOSE 8080` alone does not publish a port. |
| `SIPI_IIIF_ROOT` | Container-side media directory, e.g. `/data/images`; mounting files alone does not select a repository. |
| `--config /etc/sipi/sipi.toml` | Explicitly loads the mounted TOML file; no configuration file is discovered automatically. |
| `SIPI_PREFLIGHT_SCRIPT` | Container-side Lua file, e.g. `/etc/sipi/preflight.lua`; no policy is enabled by default. |
| `SIPI_CACHE_DIR` | Writable persistent result directory; cache is disabled if absent. |
| `SIPI_PUBLIC_BASE_URL` | Client-visible origin and optional proxy mount, not the container's internal address. |
| `SIPI_IIIF_WORKERS` | Set deliberately for container resources; automatic CPU detection is not guaranteed to reflect Docker CPU/memory quotas. |

For example, to use internal port 9090 with host port 18080, use both
`-p 127.0.0.1:18080:9090` and `-e SIPI_PORT=9090`. The public URL remains
`http://127.0.0.1:18080`. Docker options go before the image name. SIPI options,
such as `--port 9090`, go after it.

The [configuration reference](command-line.md) is authoritative for all remaining
settings, including JPEG encoding, output limits, cache capacities and CORS. Docker
CPU/memory limits are separate from SIPI's per-operation budgets; neither changes
the other automatically.

## TOML, Lua and a persistent cache

Two ready-to-run files are included:

- [sipi.toml](../examples/docker/sipi.toml) selects mounted paths, the public URL,
  two workers and the persistent cache.
- [preflight.lua](../examples/docker/preflight.lua) allows only `sample.png` and
  denies other identifiers. It demonstrates policy plumbing, not authentication.

Run from the repository root:

```sh
docker run --rm --name sipi-2-config-demo \
  --read-only --tmpfs /tmp --workdir /tmp \
  --cap-drop=ALL --security-opt=no-new-privileges:true \
  -p 127.0.0.1:18080:8080 \
  --mount "type=bind,src=$(pwd)/examples/images,dst=/data/images,readonly" \
  --mount "type=bind,src=$(pwd)/examples/docker,dst=/etc/sipi,readonly" \
  --mount type=volume,src=sipi-2-demo-cache,dst=/var/cache/sipi \
  lrosenth/sipi-2:0.1.3 --config /etc/sipi/sipi.toml
```

The volume is created automatically if absent. Repeat the first-image `curl`
commands. To see an intentional denial:

```sh
curl -i http://127.0.0.1:18080/not-allowed.png/info.json
```

That request returns 403 before looking for a file. The allowed image can reuse
cached encoded results, but every request still evaluates Lua and source identity.
Stop with Ctrl-C. Running the same command again reuses the named cache volume.
Removing a container does not remove this explicitly named volume.

To discard **only this demo cache**, first stop its container, then run:

```sh
docker volume rm sipi-2-demo-cache
```

For application use, copy/adapt the configuration and script to your own directory.
Lua and secret-file contents are snapshotted at startup. Restart after changes;
if a bind-mounted single file was replaced atomically, recreate the container to
adopt the replacement. A directory mount avoids that single-file inode problem,
but still requires a server restart to load new contents. Read the
[Lua contract](preflight.md) before extending the policy.

## Compose example

The existing [Compose file](../docker/compose.example.yaml) provides read-only
media/Lua mounts, a named cache volume, a read-only root filesystem, a tmpfs work
directory and loopback-only publishing. It uses environment configuration and a
single-file policy mount; it does **not** load the preceding TOML example.

From the repository root:

```sh
export SIPI_IMAGE=lrosenth/sipi-2:0.1.3
export SIPI_MEDIA_DIRECTORY="$(pwd)/examples/images"
export SIPI_LUA_SCRIPT="$(pwd)/examples/docker/preflight.lua"
export SIPI_HOST_PORT=18080
export SIPI_PUBLIC_BASE_URL=http://127.0.0.1:18080

docker compose -p sipi-demo -f docker/compose.example.yaml config --quiet
docker compose -p sipi-demo -f docker/compose.example.yaml up -d
docker compose -p sipi-demo -f docker/compose.example.yaml logs --tail 50 sipi
```

Use the same image/deny probes as above. Required variables and missing bind sources
fail instead of being silently replaced with defaults or created directories.
`SIPI_HOST_PORT`, `SIPI_IMAGE`, `SIPI_MEDIA_DIRECTORY` and `SIPI_LUA_SCRIPT` are
Compose interpolation variables, not extra native SIPI configuration options.
`SIPI_PUBLIC_BASE_URL` is also passed into the server. Set these variables again
in a new shell before running Compose commands for this project.

Stop and remove the project containers/network, retaining the cache:

```sh
docker compose -p sipi-demo -f docker/compose.example.yaml down
```

The cache volume is normally named `sipi-demo_cache`. `down -v` would delete the
project's named cache volume too; use it only when deliberately discarding that
cache. Each deployment should use its own project name and cache volume. Compose
uses the image already present unless a pull policy or explicit `pull` is used.

## S3 and secrets

For an S3-backed repository, set `SIPI_IIIF_STORAGE=s3` and `SIPI_S3_BUCKET`.
Select `SIPI_S3_REGION`; for MinIO/custom endpoints also configure
`SIPI_S3_ENDPOINT` and `SIPI_S3_PATH_STYLE=true`. An image-directory mount is not
needed if all selected images and masks are remote. The cache can remain local.
Use a suitable policy; the bundled sample policy is intentionally for local media.

`127.0.0.1` inside a container refers to that container. To reach another service
in a Compose network, use that service's DNS name and internal port, for example
`http://minio:9000`, not the host-published loopback port. The endpoint must exist
and be reachable; this guide does not start an S3 service. See
[S3 configuration](command-line.md#s3-credentials) and
[local MinIO development](minio-development.md).

S3 supports the SDK credential provider chain or explicit access/secret keys and
an optional session token. Host credentials are not automatically copied into the
container. Supply credentials through your deployment's chosen runtime mechanism;
never bake real values into the image or commit them in example files.

Lua secrets can use a read-only file mount and an environment variable naming its
**container path**, e.g. `SIPI_PREFLIGHT_SECRET_FILES_JWT_KEY=/run/secrets/jwt_key`.
The policy reads `sipi.secrets.jwt_key`. This mechanism is for explicit Lua secret
settings; it does not imply a generic `_FILE` option for every S3/native setting.
Secret bytes, including final newlines, are preserved exactly. See
[Lua secret-file behavior](lua-configuration.md#secret-files-and-lifetime).

## Reverse proxy and public URLs

SIPI listens on plain HTTP; terminate HTTPS at your proxy. Set
`public-base-url` to the origin and mount visible to the client. Host/Forwarded
headers do not automatically configure it. Otherwise `info.json`, redirects and
canonical URLs may point at an internal address or carry a duplicated prefix.

For an external request to `https://images.example.org/iiif/sample.png/info.json`:

| What the proxy forwards | `public-base-url` |
| --- | --- |
| `/sample.png/info.json` (strips `/iiif`) | `https://images.example.org/iiif` |
| `/iiif/sample.png/info.json` (retains `/iiif`) | `https://images.example.org` |

Forwarded prefixes are part of SIPI's public resource identity, not default storage
mapping. A host proxy may use the published loopback port. A proxy in a separate
container should use a shared Docker network and the SIPI service's internal port;
its own `127.0.0.1` is not the host or SIPI container.

Configure [CORS](iiif-http.md#configurable-cors) once consistently: wildcard access
without cross-origin credentials is the default; cookie-based viewers require an
explicit origin list, credential mode and appropriate application cookie setup.
SIPI does not issue login cookies. Internal result caching is separate from its
`Cache-Control: no-store` responses to clients.

For a complete proxy example, monitoring probes, cache recovery and maintenance,
see the [operations guide](operations.md).

## Logs, updates and troubleshooting

For a `docker run` example, inspect `docker logs sipi-2-config-demo`. For Compose,
use the `logs` command above. `/health` reports liveness/version; it is not a
repository/backend readiness check. The image does not define a Docker HEALTHCHECK
and does not bundle `curl` for in-container probes; run the shown probes from the
host or your monitoring system.

| Symptom | Check |
| --- | --- |
| Bind source missing / wrong file type | Absolute host path exists; media/config directories versus single Lua file are mounted as intended |
| Connection refused | Container is running; host/internal ports match; server listens on `0.0.0.0` inside the container |
| Port already allocated | Stop the earlier demo or select another host port and matching public URL |
| Source not configured | Configure `iiif-root` or an S3 default; mounting media alone is insufficient |
| 404 image not found | File/key matches the decoded identifier; prefixes do not automatically select subdirectories |
| 403 with demo policy | The sample policy intentionally allows only `sample.png` |
| Policy startup error | Lua path is inside the container; file readable, valid function and settings; inspect sanitized logs |
| Cache permission or lock error | Dedicated writable directory/volume; UID/GID 10001 access; no second instance using it |
| Read-only filesystem error | With read-only root, supply writable `/tmp`, work there and mount a writable cache if enabled |
| Source not deliverable / 501 | Check [format/layout restrictions](features.md#input-formats) and [resource limits](output-limits.md) |
| Redirect/info URL is wrong | Set external public URL and handle the proxy prefix exactly once |
| Changes to a single mounted file not visible | Recreate the container after atomic host-file replacement; restart to reload snapshots |
| S3 connection failure | Endpoint resolves from the container/network; credentials, bucket, region and addressing mode match |

Pin a version tag for routine use. For exact reproducibility, inspect the registry
index with `docker buildx imagetools inspect lrosenth/sipi-2:0.1.3` and use
`lrosenth/sipi-2@sha256:<verified-index-digest>` as the image reference. Do not copy
that placeholder literally. A tag/version string alone does not prove source
identity, and `latest` can move.

Before an update, record the working image reference and keep your configuration
and secrets. For Compose, set the intended `SIPI_IMAGE`, run `pull`, then `up -d`
to recreate; inspect logs, health and a representative authorized image. Roll back
by setting the recorded previous image and recreating again. Keep input images
read-only. Result caches are regenerable: if a release requires an incompatible
cache change, use a fresh dedicated volume and preserve the previous one until
rollback is no longer needed. Do not run old/new instances against one cache.

## Building your own image

Source/dependency builds are a separate workflow from using the published image:

```sh
make docker-local  # Build and test native architecture; no publication.
make docker-test   # Build and test AMD64 and ARM64; no publication.
```

An ordinary local build and its smoke test are also available:

```sh
docker build -t sipi-2:local-test .
python3 tests/docker_smoke.py sipi-2:local-test
```

`make docker-build` additionally **publishes** the version and `latest` after
verification. Use the [Makefile release guide](makefile.md#docker-build-and-publication)
for that operation; it is not needed for any runtime example above.

The build stage uses Ubuntu 24.04, Conan 2.33.0, the lockfile and local Grok recipe.
It derives the Linux lock while preserving library pins and uses a locked BuildKit
Conan cache. `BUILD_JOBS` defaults to four. The final Ubuntu runtime contains the
stripped executable/shared libraries, CA certificates and optional OLDAP helper.
It contains no C++ source, Conan cache, compiler, CMake, Ninja or Python build
environment. Example TOML/Lua files under `examples/docker/` are host-mounted;
they are not packaged into the published image. No rebuild is needed to change
runtime repositories or mounted policy files.

## OLDAP integration boundary

The packaged `/opt/sipi/examples/oldap/docker-entrypoint.sh` is an optional adapter;
the default image entrypoint does not run it. It translates dedicated OLDAP media
key/issuer/audience settings to SIPI Lua settings, preserving explicit SIPI overrides.
It sets the minimum permission to 1; `SIPI_PREFLIGHT_CONFIG_MINIMUM_PERMISSION=2`
selects full-view-only delivery. It does not use access/refresh keys.

Anonymous lookup is opt-in through `OLDAP_SIPI_ANONYMOUS=true` and `OLDAP_API_URL`.
The reserved `unknown` pseudo-user needs no password; another configured service
account requires one. Missing media tokens can use the enabled anonymous lookup;
invalid supplied tokens remain denied. See the
[OLDAP integration contract](oldap-preflight.md) and adapt that deployment's paths,
networking and secrets explicitly.

The separate oldap-mediaserver project has its own activation/rollback procedure
and proxy-prefix conventions. Its local trial instructions are not prerequisites
for this generic Docker guide; do not replace an existing deployment with a demo.
