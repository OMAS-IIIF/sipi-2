# Bounded IIIF concurrency

`IiifWorker` owns a fixed pool of joinable `std::thread`s and one FIFO queue with
at most `iiif-queue-limit` waiting requests (default 64), independent of pool size. A worker performs preflight,
source access, metadata, geometry, cache lookup, rendering and response completion.
Drogon's I/O loop remains separate; `/health` does not wait for an image worker.
Queue overload returns the existing HTTP 503 `iiif_busy` response.

Before a job starts and again before decoding on a result-cache miss, the worker
reads a shared atomic liveness token. The HTTP adapter observes connection status
on its I/O loop every 50 ms while the request is owned by a worker or its queue.
Weak timer captures stop observation after completion and never keep a connection
alive. This avoids Trantor's non-atomic status reads from workers and works on
macOS, where Drogon 1.9.13 does not forward connection lifecycle callbacks.
A delayed observation can allow unnecessary work. A false token skips remaining
work and completes with 503 `client_disconnected`, which nobody receives.
Running codec or storage work is never interrupted. Response completion is also
dispatched to the connection's I/O loop before invoking Drogon; its callback
reads connection state before its internal dispatch.

## Configuration and memory

`iiif-workers` / `--iiif-workers` / `SIPI_IIIF_WORKERS` select **1..32** workers,
using CLI > environment > TOML > default priority. Zero is invalid. The default is
`clamp(std::thread::hardware_concurrency(), 1, 4)`: logical CPUs, fallback one,
with a conservative cap of four. Startup logs the actual pool size and queue
limit. Set one to restore serial processing. Restart to change the setting.

`iiif-queue-limit` / `--iiif-queue-limit` / `SIPI_IIIF_QUEUE_LIMIT` selects
**1..1024** waiting requests (default 64) with the same precedence. Waiting jobs
hold parsed request values only, not decoded pixels; the limit bounds latency
under overload rather than memory.

The cap deliberately avoids multiplying large per-request allocations by every
CPU on a large machine. Existing raster/codec-operation limits (typically 256 MiB)
and the 64 MiB encoded-response limit still apply independently. They are NOT a
whole-process memory quota: decoder internals, metadata, vector capacity and
responses awaiting socket transmission consume additional memory. Explicit larger
worker counts require operator memory planning. Containers may report host CPU
counts; configure workers explicitly when CPU/memory allocations differ. No
physical-memory detection, dynamic scaling or automatic RSS admission is implied.

## Shared and private state

- Request/source handles, Lua states, codec instances, raster buffers and response
  objects remain request-local. Lua evaluation uses the immutable script snapshot.
- One metadata LRU retains immutable shared snapshots for the whole pool. A
  dedicated mutex protects get/put only, never source I/O or codec work. Concurrent
  cold misses may read duplicate headers; they do not multiply retained cache
  capacity. Active snapshots can outlive eviction.
- ResultCache already serializes its index/file mutations internally. Body reads
  use independent offsets. The SDK client is shared; each S3 source retains its
  own version-bound request state. Grok retains its synchronous single-thread
  codec setting, avoiding a nested pool per image; diagnostics are thread-local.

## Coordinating cache fills

After every request has independently passed authorization, source version,
geometry and capability checks, a configured result cache uses one fill mutex per
**full serialized rendering identity**. The worker holds it through lookup,
rendering and publication. Waiters then recheck the cache and normally find the
completed result. Public aliases and normalized angle spellings share this key;
credentials, HTTP headers and policy decisions are never shared.

The fill registry stores weak references and removes expired entries on each
acquisition; only current workers retain live mutexes. Distinct keys do not hold
each other's fill locks. Queue/cache/registry locks are not held during decoding.
This lock-based coordination is intentionally small: waiting requests occupy
worker slots, and repeated hot-key requests can temporarily occupy the pool.
No fairness guarantee or async continuation scheduler is introduced.

With the result cache disabled, requests render independently. If a fill fails or
is not admitted, the next waiter may retry. Cache errors continue to fall back to
normal delivery, and locks are released by RAII on all exits. There is no cache of
errors or additional memory cache of encoded responses.

## Lifecycle and verification

All shared members are initialized before threads start. Partial thread-start
failure stops and joins already-started workers. The owner calls `stop()` after
the HTTP loop exits: reject new requests, discard queued jobs, wake every idle
worker and join all active jobs before shared state is destroyed. Active codec/I/O
work is not forcibly cancelled. Call stop/destruction from the owner, never a
completion callback; lifecycle calls are not concurrent. Callback exceptions are
contained so one bad completion cannot terminate a worker or the process.

`iiif-worker` proves four-worker overlap by holding four callbacks simultaneously,
then checks the default and a configured queue limit, delivery, shutdown rejection,
invalid counts and the disconnected-client skips before policy and before rendering.
Four simultaneous cache-body failures verify successful uncached delivery and one
operator error across the pool.
`worker-queue-http` repeats the disconnect and queue-limit checks through the real
listener, with one worker held by a gated Lua HTTP call.
`parallel-http` compares concurrent TIFF/J2K JPEG/PNG bytes with serial results,
checks same-key cache fills, per-request denial and capacity-rejected retries.
MinIO tests exercise concurrent S3 requests through the shared client. Configuration
tests cover defaults, range validation and CLI/environment/TOML precedence.

Validation of this change: all 34 Debug suites pass, including MinIO; selected
AddressSanitizer/UndefinedBehaviorSanitizer suites pass. The direct worker test,
including real TIFF/J2K rendering and shared caches, also passes ThreadSanitizer.
An initial full HTTP ThreadSanitizer run with uninstrumented Drogon/Trantor
reported callback/queue handoff races. Rebuilding both HTTP libraries with TSan
removed those reports: the parallel HTTP test passed four runs without suppressions,
and the direct worker/render/cache test also passed. This supports the explanation
of synchronization invisible to the earlier partial instrumentation. Other native
dependencies remain uninstrumented; this is not a proof of every path/interleaving.
See [the reproducible build and workload](benchmarks.md).
