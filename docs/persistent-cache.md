# Persistent encoded-result cache

`src/cache/ResultCache.h/.cpp` implements a local file store with a SQLite index.
`CacheKey` determines rendering identity; `ResultCache` stores **complete encoded
bytes** and output metadata. It has no knowledge of Lua, HTTP, image decoders or
source authorization. Source metadata reuse is provided by a separate [in-memory cache](metadata-cache.md).

The executable initializes the configured store and shares ownership with the IIIF
worker. JPEG/PNG image requests now use authorized lookup/fill for both local and
S3-backed TIFF/JPEG 2000 sources. A hit skips pixel decoding, resizing and encoding;
source opening/version checks still run, while unchanged headers can come from the metadata cache. Browser/proxy responses
retain `Cache-Control: no-store`; server-side reuse is independent of that policy.

## Configuration and layout

```toml
cache-dir = "data/cache"
cache-max-bytes = 1073741824
cache-target-percent = 90
```

Corresponding CLI options are `--cache-dir`, `--cache-max-bytes`, and
`--cache-target-percent`; environment names are `SIPI_CACHE_DIR`,
`SIPI_CACHE_MAX_BYTES`, and `SIPI_CACHE_TARGET_PERCENT`. Each follows
CLI > environment > TOML > default. The directory has no default (disabled);
capacity defaults to 1 GiB and cleanup to 90%. Relative paths use the process
working directory, including when configured in TOML. The server creates missing
directories. See [configuration](command-line.md) for ranges and validation.

```text
<cache-dir>/
    cache.lock
    cache.sqlite3
    cache.sqlite3-journal       # transient SQLite rollback journal
    objects/
        ab/
            ab…<64 hex characters total>….bin
            .tmp-XXXXXX        # exists only during publication
```

The filename is derived from the digest, with its first two characters as the
shard. MIME type comes from the index, not the `.bin` extension. No public URL,
identifier, user-supplied filename or stored relative path controls file access.
The index stores digest, full serialized identity, byte count, MIME type, encoded
width/height and last-access timestamp (Unix microseconds). Full source dimensions
cannot be recovered from output dimensions; this is not a source-header cache.
Source/version associations currently reside in the serialized identity; explicit
source-indexed purge and metadata tables are not implemented.

Use a private, dedicated **local filesystem** directory, separate from the image
repository. One `ResultCache` owns it at a time. A nonblocking OS `flock` on
`cache.lock` excludes other instances/processes and releases on exit or crash.
Do not remove the lock file or edit cache files while the owner runs. SQLite alone
would not coordinate file publication/cleanup across independent owners.
Shared network directories and distributed cache operation are unsupported.
A container needs persistent storage mounted at this directory to retain results.

## Public API and ownership

```cpp
sipi::cache::ResultCache cache("data/cache");
// key is derived from an already authorized, validated rendering plan.
if (auto hit = cache.get(key)) {
    // hit->metadata describes the encoded image.
    // hit->bytes is an owning shared_ptr<const ByteSource> for bounded reads.
} else {
    // Render completely and successfully first, with the existing response limit.
    const sipi::cache::ResultMetadata metadata{"image/png", width, height};
    const bool admitted = cache.put(key, metadata, encoded_bytes);
    // false means deliver normally without storing the result.
}
```

`encoded_bytes` is a borrowed `span<const byte>` valid throughout `put`. Only the
completed output is accepted; there is no second in-memory copy or retained span.
The caller must supply metadata matching the bytes and key. The store accepts
JPEG/PNG media types and positive dimensions/byte counts; it does not decode or
validate image content. It does not yet provide an incremental encoder sink.

`get` returns `nullopt` for absent/missing/wrong-size files. A hit owns a read-only
file descriptor through `ByteSource`; independent offset reads are thread-safe.
Reads can outlive the store. Within the live owner, outstanding handles **pin**
entries against eviction, so reclaimed bytes actually leave the filesystem.
Finish deliveries before destroying/reopening the owner for predictable budget
accounting; a handle still survives an unlink through POSIX file semantics.

One mutex serializes store operations, including the single possible temporary
write. Returned file reads run independently. Encoding remains outside the store
and outside SQLite transactions. Two puts of the same key preserve the first
published result; incompatible metadata/length or unequal serialized identity
under the same digest raises `CacheError`, never overwrites it. Equal identity,
metadata and length assume deterministic rendering; bodies are not compared.
This prevents duplicate stored entries but does not coalesce image generation.

## Publication and recovery

1. Check the full identity on an existing entry; reject digest collisions.
2. Admit against capacity, pinned entries and free-space headroom.
3. Write a uniquely created temporary file in the final shard directory.
4. Synchronize file data, close successfully, rename atomically, sync the directory.
5. Commit the complete ready entry to SQLite in a short transaction.

SQLite uses DELETE journaling and FULL synchronization. The image filesystem and
SQLite remain two separate resources: a crash between rename and commit can leave
an orphan. Before accepting calls, constructor recovery removes abandoned `.tmp-*`
files and unindexed hash-named bodies, and drops rows with missing/nonregular or
wrong-sized bodies. It only traverses the reserved hash namespace and never follows
shard symlinks. Unrelated filenames are preserved. After publication failure,
further puts return false until reopen/reconciliation; uncertain orphan usage
cannot lead to continued unaccounted admissions. Reads remain available.

The schema has `application_id = 0x53495049` (SIPI) and `user_version = 1`.
Foreign/nonempty unidentified databases and unsupported versions fail initialization
without reset or destructive migration. A corrupt index is not automatically
recreated. Operators can select a new empty cache directory; source images remain
authoritative. Same-sized external body corruption is not detected: this is not a
content-integrity validator. Normal atomic publication protects against partial
encodings; external modification of an active cache is outside the contract.

Sync ordering and recovery aim at consistent misses after failures, not a guarantee
that every recent fill survives power loss on every filesystem/device. Recovery
may discard otherwise usable cache data. Ordinary stop/start and abrupt process
exit after committed publication are covered by tests.

## Capacity and approximate LRU

The maximum counts ready encoded bodies plus the one possible in-progress body.
Before a write would exceed it, evict oldest entries until existing bytes plus the
incoming body fit the lower target. Pinned entries are skipped; if the target
cannot be reached, decline admission. Earlier evictions can still have occurred.
An individual body above the maximum is declined. A body above the lower target
but within the maximum may occupy the cache alone. Reopening with a smaller limit
triggers cleanup before requests can use the store.

Last-use timestamps accumulate in memory and flush in a transaction after 64 hits,
before eviction, on explicit `flush_accesses()`, or best-effort on destruction.
A crash may lose recent touches; this affects eviction ordering, not correctness.
Wall-clock timestamps make this approximate LRU, including during clock changes.
There is no background timer or maintenance thread.

Database, rollback journal, directory blocks and allocation-unit overhead are
**additional** to the encoded-byte maximum. Admission checks available filesystem
space for the incoming body plus 64 MiB headroom. This is not a disk quota or an
exclusive reservation against other processes; database growth and concurrent
external use may still cause I/O failure. Cache failures must remain recoverable
through uncached delivery. A filesystem quota can bound total physical use where
operational requirements demand it.

## Errors, tests and next step

`CacheError` represents database/file/collision failures and invalid API inputs;
filesystem and allocation exceptions may propagate. `ByteSourceError` can arise
later while a caller reads an opened hit. Constructor failure releases acquired
resources. Destruction never throws. Do not race destruction with store methods.
At startup, invalid selected config is fatal; unavailable storage warns and permits
uncached server operation. HTTP lookup/publication has separate exception boundaries:
cache failures fall back to rendering/delivery, while policy/source/codec failures
retain their normal HTTP error responses.

The CTest `result-cache` suite verifies binary/metadata round trips, source-version
misses, duplicate publication, locking, restart, abrupt process exit, incomplete
publication recovery, injected index errors, collision checks, unsupported schema,
LRU/hysteresis, pinned reads, capacity changes and concurrent callers.
`startup-configuration` checks all config layers and validation; `cache-startup`
launches the real server to verify environment wiring, relative paths, restart,
help without I/O and graceful unavailable-cache fallback.

`cache-http` verifies actual hit logs and unchanged output bytes, equivalent URLs,
Lua denial/failure and source overrides, GET/HEAD, restart, atomic source replacement,
missing sources, missing/truncated bodies, unsupported requests and failed encodings,
admission rejection and injected lookup/publication failures. `s3-http` additionally
checks cached S3 responses and replacement/deletion under an unchanged IIIF URL.

## HTTP integration

The order is: parse -> preflight -> resolve/open definitive version-bound source ->
reuse/read version-bound metadata -> resolve geometry -> check current source layout and requested
operations -> construct key -> lookup. There is no shortcut around authorization,
source existence/version or today's renderer capabilities. `info.json` and errors
are not stored in the encoded-result cache.

The worker uses variant `original`, because today's Lua contract only supports
allow/deny and source selection. Future virtual/restricted variants must pass their
actual effective variant identity. JPEG quality 90 is supplied explicitly to both
the key and encoder; PNG uses the current fixed recipe. Source layout selects the
same native-gray quality normalization used by delivery.

On a hit, compare stored MIME/dimensions to the current output plan, then read the
opened handle into the existing bounded response string (maximum 64 MiB). Complete
reading must succeed before returning 200. A cache read failure discards those
bytes and renders from the already opened source. On a miss, only a successful
complete encoding reaches `put`; admission rejection or publication failure still
delivers that encoding. A lookup failure skips publication for that request.
Shared response construction keeps content type and `Cache-Control` identical.

GET and HEAD share a key. As before, Drogon suppresses the HEAD body during
transmission; HEAD currently reads/renders the same representation and may fill
the cache. There is no new HTTP cache-status header; debug logs report hit/miss,
stored/declined without keys, source paths or credentials. Warning logs identify
lookup/publication fallback without private diagnostics.

The bounded IIIF pool coordinates lookup/render/publication with a separate mutex
per full serialized result identity. A waiting request rechecks the cache after
the previous fill completes. Different results render concurrently. Failed or
capacity-declined publication permits retries; no response/error is shared in
memory. Coordination is enabled with the persistent cache only. See
[worker concurrency](worker-pool.md) for ownership, limits and shutdown behavior.
Cache reads/writes remain off Drogon's I/O loop. Streaming/backpressure is separate.

SQLite API references: [opening connections](https://www.sqlite.org/c3ref/open.html),
[application/version and durability pragmas](https://www.sqlite.org/pragma.html).

For [orthogonal transforms](orthogonal-transforms.md), indexed width/height
describe the final encoded image (swapped for 90/270 degrees). Capability checks
precede lookup; cached output never enables unsupported angles.
