# Local MinIO for S3 development

This optional Docker Compose environment prepares S3-compatible storage for the
[S3ByteSource](s3-byte-source.md). HTTP delivery can select local files or S3; normal builds and CTest
runs do not require Docker or MinIO.

## Storage choice

Use a Docker-managed named volume (`sipi-2-minio_data`) mounted at `/data`.
On macOS, its contents live in Docker Desktop's Linux VM disk. Data survives
container recreation, stop/start, and `docker compose down`. It is not stored in
the container's disposable writable layer or in the Git working directory.
Docker Desktop reset, volume deletion or `down --volumes` removes these test data.

A host bind mount is unnecessary: upload/download objects through S3 or the web
console, rather than manipulating MinIO's internal files. This also avoids a
machine-specific host path and host filesystem permission requirements.

## Start and use

Start Docker Desktop first. From the SIPI-2 repository root:

```sh
docker compose -f dev/minio/compose.yaml up -d --build --wait
```

The first run compiles MinIO and downloads Go dependencies; allow several minutes
and an internet connection. Later starts reuse the built image and data volume.
The configuration binds both ports exclusively to IPv4 loopback. If 9000/9001
are already occupied, change the host-side port numbers in the Compose file.

- S3 endpoint: <http://127.0.0.1:9000>
- Object browser: <http://127.0.0.1:9001>
- Access key / username: `sipi-test`
- Secret key / password: `sipi-local-test-password`
- Region: `us-east-1`
- Planned test bucket: `sipi-test-images`

These are deliberately public development credentials, not production secrets.
Log into the object browser, create `sipi-test-images` and upload a TIFF. Existing
fixtures can be generated with:

```sh
cmake --build --preset conan-debug
ctest --preset conan-debug -R '^http-tiff-fixtures$' --output-on-failure
```

For example, upload `build/Debug/http-test-data/tiled.tif` as `tiled.tif`.
A fresh volume starts empty; bucket creation and fixture upload are currently manual.
The initial laptop setup already contains `sipi-test-images/tiled.tif`.
Keep the bucket private. The opt-in S3 integration test provisions separate disposable buckets and tests
range reads and object versioning automatically; see [the adapter tests](s3-byte-source.md).

## Inspect, stop and restart

```sh
docker compose -f dev/minio/compose.yaml ps
docker compose -f dev/minio/compose.yaml logs --tail 50 minio
curl --fail http://127.0.0.1:9000/minio/health/ready
docker compose -f dev/minio/compose.yaml down
docker compose -f dev/minio/compose.yaml up -d --wait
```

`down` preserves uploaded objects. To intentionally erase **all MinIO test data**,
use `docker compose -f dev/minio/compose.yaml down --volumes` instead. No automatic
restart policy is configured, so this development service does not start itself
with Docker Desktop.

## Image provenance and verification

The [MinIO community repository](https://github.com/minio/minio) is archived and
now documents source-only distribution. The former official image tag could not
be retrieved while preparing this setup. We build upstream release
`RELEASE.2025-10-15T17-29-55Z`, pinned to commit
`9e49d5e7a648f00e26f2246f4dc28e6b07f8c84a`, using its Go 1.24.8 toolchain.
The runtime runs as UID/GID 10001. This is an isolated development fixture, not a
recommendation to deploy unmaintained MinIO in production. SIPI-2 targets S3, not
a dependency on this particular server.

The source commit is fixed; base image tags and Debian package repositories are
not digest/snapshot pinned, so rebuilds are not bit-for-bit reproducible.
Verified on Docker Desktop (Linux ARM64): Compose validation, source build,
container readiness, object-browser HTTP response, authenticated bucket creation
and TIFF upload, byte-range equivalence, and byte-identical full download after
container recreation. Anonymous object access returns 403. Browser login was not
automated; S3 authentication was verified directly.

For delivery from the manual test bucket, run `./build/Debug/sipi-2 --config examples/minio.toml`. `/museum/tiled.tif/info.json` reads key `tiled.tif`: the URL prefix does not participate in default storage mapping.
