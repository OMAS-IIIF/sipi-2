# Reproducible local tile workload

This benchmark evaluates IIIF worker concurrency, not general TIFF-versus-JPEG 2000
quality, compression or archival suitability. It does not change server behavior.
Run on an otherwise quiet machine, outside the IDE debugger and without parallel
builds, sanitizers or other load tests. Ordinary desktop activity can still affect
results; there is no CPU affinity, turbo control or filesystem-cache flushing.

Recorded run: [2026-10-05 local measurements](benchmarks/2026-10-05-local.md).

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
