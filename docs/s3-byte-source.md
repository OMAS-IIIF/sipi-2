# S3 byte sources

`src/storage/S3ByteSource.h` implements the existing `ByteSource` contract through
AWS SDK for C++ 1.11.692. The `s3storage` target keeps all AWS types private. The
TIFF reader is unchanged. Source selection is integrated with HTTP, configuration and Lua; see
[repository selection](command-line.md#repository-selection-local-or-s3).

## Connection and source ownership

`S3Client` owns a shared SDK client/connection pool. Construct it once per backend
configuration and reuse it for many `S3ByteSource` objects. Sources retain the
shared implementation; the client facade may go out of scope before the sources.
Concurrent reads use independent response streams and the same connection pool.

The module initializes the SDK once, using a function-static RAII runtime. Do not
call `Aws::InitAPI`/`ShutdownAPI` elsewhere. Finish all requests and destroy clients
before process teardown. Returned metadata and pixel buffers do not borrow SDK
objects or source storage.

```cpp
#include "storage/S3ByteSource.h"
#include "imgformats/TiffReader.h"

sipi::S3Config config;
config.endpoint = "http://127.0.0.1:9000";
config.region = "us-east-1";
config.path_style = true;
config.access_key = "sipi-test";                 // local test credentials only
config.secret_key = "sipi-local-test-password";
const sipi::S3Client client(config);
const sipi::S3ByteSource source(client, "sipi-test-images", "tiled.tif");
const auto metadata = sipi::read_tiff_metadata(source);
const auto crop = sipi::read_tiff_region(source, {50, 30, 100, 80});
```

An empty endpoint selects AWS's regional endpoint. An explicit endpoint is an
HTTP(S) origin without a path, query or embedded credentials. TLS certificate
verification remains enabled. Ordinary bucket names and literal object keys are
used; the SDK handles key encoding. Access-point ARNs are outside this adapter's
contract. For AWS, virtual-host addressing is the default; MinIO uses path style.
Region selection is explicit and does not probe EC2 metadata. Empty access/secret
keys select the SDK's default credential provider chain, which may contact
credential services when used outside the explicit-credential local tests.
Startup credentials can now be supplied through CLI/environment/TOML in
`ServerConfig.s3`; see [configuration parameters](command-line.md#s3-credentials).
Endpoint/bucket selection is configured server-side; Lua may select a key in that bucket.
Explicit credentials may include a session token; do not place credentials in
public URLs, source identities or cache keys.

## Opening and consistent reads

Opening calls `HeadObject`, captures object size and ETag, and captures VersionId
when present. `SourceInfo.identity` combines backend endpoint, region, bucket and
key using length-framed components. The identity is internal, not a public URL.

- With a nonempty, non-`null` VersionId, all GETs specify that version.
- Without such a version, all GETs carry `If-Match` with the captured ETag.
- Versioned requests also carry `If-Match`; replies must match the captured ETag
  and, when pinned, VersionId.
- The literal `null` version is mutable (for example with suspended versioning),
  so it follows the conditional-read path.

ETags are opaque server validators, not hashes we interpret. For unversioned
objects this is conditional consistency under the server's validator semantics,
not retention of old bytes. Deleting a pinned version can still make it unreadable.

`read_at` fills the requested destination up to captured EOF. It splits large
reads into at most 1 MiB HTTP ranges and writes into the caller's bounded buffer,
without accumulating an entire object in an SDK string stream. Content range,
length, received byte count and source validators are checked before returning.
After an exception the caller must discard the buffer, as for any ByteSource.
Offsets at/beyond EOF and empty buffers require no GET.

Current request policy: 2-second connection timeout, 10-second HTTP request
timeout, redirects disabled, and SDK retry count zero. These limits apply per
request, not to an entire multi-range read or credential acquisition. A later
server-level deadline/cancellation policy must cover the whole operation. No
read-ahead or range cache is included; repeated TIFF header reads still cost
network requests. Measure before adding bounded buffering/coalescing.

## Errors

Missing objects at open produce `not_found`. Failed preconditions and objects
removed after opening produce `changed`. Other transport, authentication and
protocol failures produce `io`; invalid arguments produce `invalid_argument`.
Errors use generic messages rather than SDK diagnostics containing storage keys
or endpoints. Allocation failures may propagate. The adapter returns no HTTP
response itself; eventual route integration will map these errors.

## Build and tests

Conan enables the S3 service module and disables the recipe's default auxiliary
services. SDK core/CRT and HTTP/crypto dependencies remain required; disabling
service modules does not remove those foundation libraries. Existing locked
versions are retained while adding the SDK dependency graph.

Normal CTest includes an offline loopback HTTP fixture (`s3-protocol`) that checks
ignored/wrong ranges, truncated bodies, bad validators, authorization failures,
missing objects and precondition failures. It does not require Docker or contact
AWS. To include the real MinIO integration test:

```sh
docker compose -f dev/minio/compose.yaml up -d --wait
conan install . -pr:h profiles/macos-debug -pr:b sipi-2-local --build=missing
cmake --preset conan-debug -DSIPI_TEST_S3=ON
cmake --build --preset conan-debug
ctest --preset conan-debug --output-on-failure
```

The opt-in setting is cached; set `-DSIPI_TEST_S3=OFF` to return to the default
Docker-independent suite. `s3-byte-source` uses only the fixed local MinIO endpoint
and public test credentials. It creates uniquely named disposable buckets,
uploads fixtures, tests versioned/unversioned/suspended-versioning replacement,
EOF, multi-range/concurrent reads, and compares TIFF metadata/full image/crops
against local reads. Cleanup removes its objects, versions, delete markers and
buckets; it does not modify `sipi-test-images`. A stopped MinIO is a test failure,
not a skipped integration test.

Verified on macOS against local MinIO: all 14 suites (with integration enabled),
plus S3 tests and their TIFF fixture under ASan/UBSan. Conan dependency binaries
are not sanitizer-instrumented. Actual AWS, HTTPS endpoints, production credential
providers and remote-network performance remain unverified.

Typed local/S3 selection and preflight are integrated; authorization precedes
source opening. The opt-in `s3-http` suite compares local/S3 JPEG bodies, checks
info/HEAD, prefix mapping and mixed Lua sources. Next, measure and improve request
counts with bounded buffering; keep result caching separate.
