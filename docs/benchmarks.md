# Reproducible local tile workload

For the four-server Docker comparison, see [server comparison](server-comparison.md).

This benchmark evaluates IIIF worker concurrency, not general TIFF-versus-JPEG 2000
quality, compression or archival suitability. It does not change server behavior.
Run on an otherwise quiet machine, outside the IDE debugger and without parallel
builds, sanitizers or other load tests. Ordinary desktop activity can still affect
results; there is no CPU affinity, turbo control or filesystem-cache flushing.

Recorded run: [2026-10-05 local measurements](benchmarks/2026-10-05-local.md).

For a real OpenSeadragon trace through Caddy and the OLDAP Lua policy, see
[2026-10-07 viewer and authorization measurements](benchmarks/2026-10-07-oldap-viewer.md).
That separate workload compares anonymous permission lookups with media-token
validation using the existing local deployment and warmed image URLs.

## Release build and fixtures

From the repository root on the current macOS development setup:

```sh
conan install . -pr:h profiles/macos-debug -pr:b sipi-2-local -s:h build_type=Release \
  -c tools.cmake.cmaketoolchain:user_presets= --build=missing -of build/benchmark-deps
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/build/benchmark-deps/build/Release/generators/conan_toolchain.cmake"
cmake --build build/release --target sipi-2 benchmark-images -j 8
build/release/benchmark-images build/benchmark-images
```

The separate Conan output directory leaves the CLion Debug preset unchanged.
Dependencies as well as SIPI use Release. `benchmark-images` is excluded from
normal builds and CTest. Its output directory must be new; no existing files are
overwritten. Generation is setup work, never included in request timings.

`benchmarks/CreateImages.cpp` creates deterministic 4096 x 4096 RGB8 pixels with
gradients, edges and bounded noise, stored as:

- `master.tif`: top-left, 256 x 256 tiles, LZW plus horizontal predictor; a main-IFD
  pyramid with 2x reductions to 256 x 256. Reduced pixels average their parent.
- `master.jp2`: lossless RGB8, reversible wavelet, 1024 x 1024 tiles, six resolutions
  using the pinned Grok encoder. This is not an archival-encoding recommendation.

They share full-resolution source pixels. TIFF pyramid and JPEG 2000 reduced
resolution pixels need not match; correctness comparisons are within each format.
The synthetic texture is not representative of every photograph/manuscript.

## Run

```sh
python3 benchmarks/tile_load.py --server build/release/sipi-2 \
  --images build/benchmark-images --output build/benchmark-results \
  --repetitions 5 --clients 8 --warm-rounds 128
```

The output directory must be new. All large images, logs and raw results stay
under ignored `build/`. Python uses only the standard library plus the system
`ps` command (tested on macOS). No installed server, original source or global
machine setting is modified. Listeners use ephemeral loopback ports, inherit no
SIPI environment settings, and are stopped by the runner. A port-allocation race
with unrelated processes remains possible and causes an explicit test failure.

Each source has 128 distinct image URLs: an 8 x 8 adjacent grid at 1:1 and another
at 1:2, each producing a 256 x 256 JPEG at the server's normal quality. This models
a tile-fetch workload, not a real browser/JavaScript rendering benchmark. Eight
closed-loop clients each own a persistent connection. Client connection setup is
outside timing. HTTP latency includes server queue time and transfer/body reads;
it excludes waiting in the Python executor. Wall throughput includes per-response
SHA-256 verification and client bookkeeping, so very fast cached results can be
limited by the client, HTTP machinery or cache index rather than image workers.

A one-worker run without a result cache establishes reference SHA-256s and checks
JPEG dimensions/MIME/status. Every measured response must match its own reference.
The workload never exceeds the 64-waiting-job limit with eight active clients.

Each trial starts a fresh server and independent 512 MiB result cache. `info.json`
warms source metadata before measuring:

1. **Cold result cache:** request each of the 128 URLs once; include cache writes,
   fsync and publication. OS page caches are not cold; reference generation has
   already read the sources. This is not pure decode throughput.
2. **Warm result cache:** repeat the same URLs, normally 128 times (16,384 requests).
   The cache must contain exactly 128 entries after both phases. Same process and
   allocator state are retained from the cold phase.

Worker order (1/2/4) is deterministically shuffled in every repetition. Five
repetitions yield 60 phase records across two sources. This does not simulate
network latency, Caddy/TLS, authentication scripts, S3 or concurrent viewers.

## Results and limits

`results.json` contains binary/source SHA-256s, platform/Python/CPU facts, raw
per-request latency and size, throughput and phase summaries. `complete` stays
false until all phases pass. `summary.md` reports medians across repetitions,
throughput min/max, and the median of each trial's p50/p95 (not a pooled p95).
No timing threshold makes a test pass or fail: functional correctness does.

RSS uses `ps` at roughly 50 ms intervals and reports the maximum observed during
the phase. This is a **sampled lower bound**, not exact high-water memory or an
allocation limit. Brief phases have few samples; missing samples are reported as
such. RSS includes native code/allocator retention, excludes the load generator,
and warm RSS inherits allocations from the cold phase. Sampling adds some overhead.

Use the recorded spread and workload limits when interpreting scaling. Preserve
raw results locally; commit the compact measurement report and fixture hashes.
Do not change default concurrency based on one synthetic test alone.

## ThreadSanitizer with the HTTP dependencies

`benchmarks/build_http_tsan.py` rebuilds the pinned Drogon/Trantor sources and SIPI
with TSan in an isolated directory, retaining regular Conan binaries and presets:

```sh
python3 benchmarks/build_http_tsan.py --output build/http-tsan-instrumented
python3 tests/parallel_http.py build/http-tsan-instrumented/app/sipi-2 build/Debug/http-test-data
build/http-tsan-instrumented/app/iiif-worker-tests \
  build/http-tsan-instrumented/worker-test-data build/Debug/http-test-data
```

Prepare the normal Debug test fixtures first with CTest. The development profile
and cached Conan source trees must be available. The script uses recipe revisions
from `conan.lock`, exposes dependency headers for the rebuild, and adjusts only its
own generated CMake package paths. It supplies the same Jsoncpp target alias as
Drogon's recipe. No suppressions or changes to upstream source are used.

This instruments the application and HTTP/event-loop libraries, not every native
dependency. A clean run covers the exercised paths, not all possible interleavings,
codec internals or future functionality. Sanitized binaries are never timed.

## Watermarked tiles: local storage and MinIO

`benchmarks/watermark_load.py` reuses the persistent-client workload and byte
verification above. It compares mask-cache on/off and encoded-result-cache on/off,
for both master formats and both backends, with deterministic shuffled order.
Use the current Release build and the same generated fixtures; start the local
MinIO service before running:

```sh
python3 benchmarks/watermark_load.py --server build/release/sipi-2 \
  --images build/benchmark-images --output build/watermark-benchmark-results \
  --repetitions 3 --warm-rounds 4 --clients 8 --workers 4
```

For warm encoded-result hits, use longer phases to reduce startup/sampling noise:

```sh
python3 benchmarks/watermark_load.py --server build/release/sipi-2 \
  --images build/benchmark-images --output build/watermark-cache-hit-results \
  --repetitions 3 --warm-rounds 128 --clients 8 --workers 4 --result-cache-only
```

This focused mode measures the same sources/backends and both mask-cache settings,
with the encoded-result cache enabled throughout. Keep its results separate from
the shorter all-mode run; do not pool quantiles across different workload lengths.

The output directory must be new. The runner generates an untagged gray8
1024x512 synthetic coverage emblem (512 KiB decoded), selects white at 25% opacity
and 50% full-image width, and records source/mask/binary/runner SHA-256s. The
emblem is a benchmark fixture, not a production logo. All requests deliver 256px
JPEG tiles; the test does not measure PNG/WebP encoding or arbitrary rotation.

MinIO is deliberately fixed to `http://127.0.0.1:9000`. Both image and mask live
in a new UUID-named bucket for S3 trials. The repository's local test credentials
are defaults; override them with `SIPI_BENCH_S3_ACCESS_KEY` and
`SIPI_BENCH_S3_SECRET_KEY` if needed. Credentials are not saved in results/logs.
The runner removes only its three uploaded objects and its bucket in `finally`.
Other buckets and running application servers are untouched. Setup/upload and
cleanup are outside timings.

Each trial starts a new server. A synthetic policy leaves info requests unmarked,
so `info.json` warms image metadata without warming the mask or result caches.
This is a benchmark-only policy, not an authorization example. The initial cold
phase requests 128 distinct URLs; the first few concurrent requests may decode
the mask independently before it is retained. **Cold does not mean every request
misses the mask cache**, nor does it mean cold OS/MinIO pages. The warm phase repeats
those URLs in the same process. With the result cache disabled it still renders
every image; with the mask cache disabled it still decodes every mask. Enabled
result caches must contain exactly 128 entries after each phase.

One-worker local uncached marked responses are references for every measured
response, including S3. A central tile must also differ from an unmarked reference.
MIME, dimensions and every response checksum gate success; no throughput threshold
is used. Results are marked complete only after all trials and MinIO cleanup pass.
Raw per-request timings, sampled SIPI RSS, summaries and child-server logs remain
in the ignored output directory. RSS excludes MinIO/Docker and the Python client.
As with the local benchmark, very fast result hits can be client-limited and
short phases may have few RSS samples. Local MinIO does not model cloud latency.

Recorded run: [2026-10-08 watermark/cache measurements](benchmarks/2026-10-08-watermarks.md).

## S3 operation profiling

After rebuilding Release, run the local-MinIO profiling workload:

```sh
python3 benchmarks/s3_io_load.py --server build/release/sipi-2 \
  --images build/benchmark-images --output build/s3-io-results --repetitions 3
```

It reuses the synthetic watermark, fixture images, eight persistent clients/four
workers and owned temporary bucket from the watermark benchmark. For TIFF and JP2,
three shuffled repeats compare mask retention off/on while rendering 128 unique
tiles, then 512 warm result hits with both caches enabled. Metadata is warmed
before the measured log boundary. Warming result entries is also outside that
boundary; exact HEAD counts detect a missing/incorrect boundary. Every measured
JPEG is verified against its one-worker local reference. Warm result hits must
perform no S3 GETs, while both source HEADs remain mandatory.

With debug I/O traces enabled, the report groups operations by fixture role using
their distinct known lengths (no source paths in traces), records failures, byte
counts, p50/p95 durations and overlaps within each anonymous opened source. A
hypothetical 4 KiB page model estimates distinct pages and fetched bytes; it is
not implemented buffering, a predicted throughput gain or a memory-budget design.
No operation crosses objects/versions for overlap calculations. Metadata warmups,
initial cache filling, reference generation and uploads are excluded from counts.

Debug logging affects timings. SDK durations overlap across workers and include
more than network transit; payload bytes exclude HTTP/TLS framing. Retain the
production logging-disabled benchmarks for throughput comparisons. See
[S3 diagnostics](s3-byte-source.md#opt-in-io-diagnostics) for exact field semantics.
Results, per-operation records and child logs remain under the new ignored output
directory; only the runner's UUID bucket is cleaned up.

Recorded run: [2026-10-08 S3 operation analysis](benchmarks/2026-10-08-s3-io.md).

## Paired S3 small-read buffer comparison

Preserve the pre-change Release executable before rebuilding, then run:

```sh
python3 benchmarks/s3_io_load.py --server build/release/sipi-2 \
  --images build/benchmark-images --output build/s3-buffer-io --repetitions 3
python3 benchmarks/watermark_load.py --server build/release/sipi-2 \
  --s3-baseline-server build/s3-buffer-baseline-2026-10-08/sipi-2 \
  --images build/benchmark-images --output build/s3-buffer-throughput \
  --repetitions 3 --warm-rounds 4 --cached-warm-rounds 64 --clients 8 --workers 4
```

The comparison option restricts the matrix to S3, both source formats and enabled
mask retention. Baseline/current binaries and result-cache on/off cases are shuffled
within every repetition. Both use the **same local byte reference**, source objects,
mask and clients. Without result caching, the warm phase renders 512 tiles; with
result caching, it serves 8,192 cached tiles to make fast phases long enough.
Metadata alone is warmed before the first 128 distinct requests. Default logging
is warn, with no per-operation traces; the diagnostic run is separate and must not
run concurrently. Output never overwrites an existing directory. Binary hashes
identify the measured versions; an old executable may still depend on its original
Conan libraries, so this preserved binary is local test evidence, not a deployment
artifact. See [measured results](benchmarks/2026-10-08-s3-buffer.md).
