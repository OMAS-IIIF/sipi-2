# In-memory source metadata cache

`src/cache/MetadataCache.h/.cpp` implements a small LRU shared by the IIIF worker pool. It stores the existing `TiffMetadata`, `J2kMetadata`, `JpegMetadata` or `PngMetadata` in an
`ImageMetadata` variant, exposed as `shared_ptr<const ImageMetadata>`.
It caches source facts, not requests, authorization decisions or info.json bodies.
No codec, filesystem or database calls occur inside the cache class.

## Request order and versioning

1. Execute preflight and resolve the definitive local/S3 source.
2. Open the source and obtain its current `SourceInfo` identity, version and size.
3. Look up exactly that triple in the metadata cache.
4. On a miss, probe the signature and read the format's metadata through the same
   opened source. Admit only a successful result; failures/unknown formats are not
   cached. Oversized metadata is still usable for this request without retention.
5. Build info.json for the requested public identity, or resolve image geometry,
   check capabilities and use the existing encoded-result cache/rendering path.

A cache hit skips the signature probe and application-level header reader.
A local open/stat or S3 metadata/version request is still required; missing or
inaccessible sources never use cached metadata as a fallback. Replacement under
the same public ID changes the key. Identity includes the backend, and version and
byte size participate independently. Atomic replacement of local files and S3
version/conditional read rules still apply; in-place editing is unsupported.
Different public aliases of the same definitive source can share metadata.

Image readers may still parse codec headers internally when decoding pixels on an
encoded-result miss. A hit in both caches skips source data reads in the worker
altogether after opening/version checking, then loads the encoded cache body.
info.json is assembled afresh: its URL and current authorization are never reused
from another request. Current capability checks still apply on every image request.

## Ownership and bounds

Defaults are **256 entries and 64 MiB of accounted resident storage**. Configure
`metadata-cache-max-entries` and `metadata-cache-max-bytes` through CLI, environment
or TOML; see [startup settings](command-line.md#metadata-cache-limits). Either limit
set to zero disables admission. `MetadataCacheLimits` defines the shared defaults
and carries resolved settings through startup/server/worker into the cache constructor. The metadata cache is active independently of `cache-dir`; that option
continues to control only the persistent encoded-result cache.

Accounting includes the variant object, list entry/link estimate, source string
capacities, ICC vector capacity, TIFF directory capacity and nested extra-sample
arrays, or JPEG 2000 component capacity. JPEG headers retain their ICC vector in
the same variant/accounting path; no format-specific cache is introduced. Saturating arithmetic rejects overflow.
Allocator/control-block overhead is additional; this is not a process RSS quota.
Each entry is charged independently even if callers deliberately share one value
between multiple keys. Hits need no deep copies. A too-large entry is declined
without evicting useful existing entries.

A list holds entries in most-recently-used order. Lookups scan at most the configured
entry count (256 by default); a hit splices the existing node to the front. Admission
evicts from the back until both budgets fit. This keeps one ownership structure
without a duplicate hash index for a small bounded cache. Successful header reads
are valid independently of pixel-decoder support; such metadata is useful for
info.json even when an image operation will return 501.

Published values must be immutable, including through other aliases. The worker
constructs `shared_ptr<const ImageMetadata>` directly. Retained snapshots survive
LRU eviction or destruction of the cache; that caller-retained memory is additional
to resident accounting. Each active worker may retain its own current request snapshot. Constructor/allocation failures may throw; the worker handles
admission failures by continuing with the already-read metadata.

The cache class requires serialized access. `IiifWorker` protects its shared
instance with a dedicated mutex around get/put, independently of queue and result
cache synchronization. Source I/O and header decoding happen outside this lock;
concurrent cold misses can read the same metadata independently. Admission keeps
the first published value. The configured capacity is pool-wide, not multiplied
by the worker count. Shutdown joins all workers before destroying the cache.

## TIFF planning and persistence

Cached TIFF metadata keeps all reader-reported compatible pyramid directories.
Request planning builds a local directory-only copy with currently supported
levels. It neither erases levels from the shared value nor copies the ICC profile
for planning. Resolution selection and output pixels remain unchanged.

There is no serialization, SQLite metadata table, TTL or background thread.
Old source versions become normal LRU eviction candidates. On restart, the first
request reads each source header again; persistent encoded results remain usable
after validation. No rendering-recipe bump is required because pixel processing
and encoding semantics are unchanged.

## Tests

`metadata-cache` checks independent identity/version/size separation, immutable
shared hits, count/byte LRU eviction, ICC capacity and nested TIFF/J2K allocations,
oversized/disabled admission, invalid input and snapshot lifetime after eviction
or cache destruction. `metadata-http` checks reuse across info/image/HEAD/aliases,
authorization, unchanged TIFF planning, format replacement under the same path,
source deletion, non-cached read failures and cold startup without a result cache.
The existing result-cache HTTP and MinIO suites also run through this new path,
including changed S3 objects, failed policy and source overrides.

PNG entries retain original color/depth, interlace/transparency flags, explicit sRGB intent and ICC bytes;
ICC capacity is charged by the same visitor. They do not imply pixel capability.
