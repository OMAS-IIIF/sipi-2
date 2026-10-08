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

## Bounded small-read buffering

Each newly opened `S3ByteSource` owns a small-read LRU tied to its captured
ETag/VersionId and size. Reads whose EOF-clipped length is at most **4 KiB** use
aligned 4 KiB pages; a read crossing a page boundary fetches at most two pages.
Only complete, validated pages are admitted. The final page is clipped at the
captured EOF. At most **16 pages (64 KiB payload)** are retained, plus one incoming
page during a fill; list/vector/mutex/shared-ownership overhead is additional.
Page payload is allocated only when used, not on HEAD or a result-cache hit.
These fixed implementation bounds do not introduce new configuration options.

Larger reads bypass the LRU, preserving exact ranges and the existing 1 MiB
maximum per GET; they neither populate nor consume small-read pages. This avoids
turning large JPEG2000 reads into many small requests. A per-source mutex protects
small reads, including a miss/fill, so concurrent misses for the same page issue
one GET. Large reads release that mutex before network I/O and remain concurrent.
Copies of one source share the same pinned state/cache; independent openings never
share it. Destruction must still not race reads.

HEAD runs on **every new source opening**. Every actual GET retains the existing
If-Match and optional VersionId checks and validates Content-Range, length and
received bytes before admission. A page hit serves bytes already obtained from
that opened version; it does not perform another remote freshness check. Thus a
replacement/deletion during an ongoing read can remain unobserved until a miss
or a new opening, while all returned cached bytes still belong to the captured
snapshot. Once a remote version/change error is observed, later nonempty reads
on that source fail, including hits; the cached pages are cleared. Concurrent
reads already in flight may complete with bytes from the captured version.

Transient I/O or malformed-range failures do not admit partial pages and may be
retried by a later explicit read. Automatic SDK retries remain disabled. A read
spanning pages can have written earlier bytes when a later page fails; callers
must discard the entire destination after an exception, as required by ByteSource.
This buffer never crosses HTTP-request/source lifetimes or changes result-cache
identity/rendering recipes. Newly requested images/masks still resolve and open
the current source before higher-level cache lookup.

The [measured comparison](benchmarks/2026-10-08-s3-buffer.md) records I/O counts,
extra payload and throughput against the previous Release binary. Debug traces
still report actual SDK operations only, not cache hits or logical read_at calls.

## Opt-in I/O diagnostics

At `--log-level=debug`, each actual SDK HEAD/Range GET records one `S3 io` line:

```text
S3 io op=GET source=7 object_bytes=12345 offset=4096 requested=1024 received=1024 elapsed_us=850 accepted=1
```

`source` is an anonymous process-local number assigned when an object is opened
while debug is enabled. It correlates that HEAD with GETs from the same opened
source; it is not a persistent cache identity. Enable debug before opening sources
for complete correlation. Logs never include bucket/key, endpoint, ETag, version
ID, credentials or response body. Do not interpret the numeric ID across restarts.

`requested` is the bounded Range length; `received` is the number of bytes written
into the response buffer, including partial/error replies. It excludes HTTP/TLS
headers, TCP overhead and data rejected by the bounded stream, so it is **not wire
traffic**. HEAD has zero requested/received body bytes. `object_bytes` is the
captured object length, or zero when HEAD cannot validate it. `accepted` is one
only after the existing response/version/range checks pass. Failures are logged
during exception unwinding and still follow the existing error policy.

`elapsed_us` uses the steady clock around the synchronous SDK call and subsequent
response validation/metadata handling. It includes SDK signing/pool/network/server
work and scheduling, not just network transit; request construction precedes it
and its own log write follows it. Concurrent operation durations overlap and must
not be summed as wall time. Diagnostics catch their own logging exceptions.
When debug is off, no operation clock reads or formatted trace messages occur;
only the level check and small stack bookkeeping remain. The diagnostic itself changes no retry,
conditional-request or source-freshness behavior. Small-read buffering is described
separately above.

Use [the S3 I/O profiling workload](benchmarks.md#s3-operation-profiling) to group
counts, sizes, durations and overlaps. Debug logging changes timing/throughput;
these traces are not comparable with logging-disabled throughput benchmarks.
