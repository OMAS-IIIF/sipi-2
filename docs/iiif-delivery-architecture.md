# IIIF delivery architecture

Recorded 2026-10-03 after the architecture discussion with the owner.
This document describes the agreed direction. See docs/http-server.md for the
implemented preflight/source/metadata/geometry checks and the remaining delivery limitations.
Upload/import remains a separate, deferred concern.

## Originals, delivery sources, and output (agreed 2026-10-04)

SIPI-2 prioritizes high-quality IIIF presentation with an explicit supported input
contract. It does not promise archival fidelity through transformed IIIF output.
Separate three responsibilities:

- Preserve the original file unchanged, including its sample precision, color
  profile, and embedded metadata. Original storage and an authorized original-file
  download belong to later work; an IIIF TIFF response is not an original download.
- Prepare a normalized delivery source during future upload/import. Prefer
  pyramidal tiled TIFF and retain the full spatial resolution unless a separate
  policy explicitly requires reduction. TIFF, JPEG 2000, JPEG, and PNG remain
  planned repository formats, but accepting a container does not imply accepting
  every pixel model, bit depth, or layout it can represent.
- Initially build the complete delivery pipeline around RGB8. Target color output
  is sRGB with 8 bits per channel; add alpha, grayscale, and bitonal behavior in
  explicit increments. Output encoding determines whether alpha can be retained.

A museum's 16-bit TIFF can therefore become accessible through an imported
presentation derivative while the original retains its full precision. Direct
delivery from arbitrary 16-bit, CMYK, Lab, or other source variants is not a current
requirement. Add direct 16-bit source processing only for a concrete use case;
limiting output to 8 bits alone would not remove source-decoding/conversion complexity.

Normalization must be color-managed and reproducible. Perform color conversion
with sufficient precision before quantizing to 8 bits; assigning an ICC profile
does not convert pixels. Define missing/invalid-profile handling, alpha semantics,
orientation handling, and the exact supported repository input combinations before
implementing import. Reduced public access variants remain a separate policy,
not a side effect of normalization.

These are architectural decisions, not implemented capabilities. The current
RGB8 TIFF crop decoder retains source ICC bytes without converting to sRGB.
Tiled or strip-based gray/RGB/RGBA8 TIFF-to-JPEG/PNG HTTP delivery with pyramid selection is implemented; upload/import and
original preservation/download remain pending. See [JPEG delivery](jpeg-delivery.md).
RasterImage8 now explicitly represents gray8, RGB8 and straight RGBA8; see [alpha delivery](alpha-delivery.md).

## Repository formats and S3-ready reading (agreed 2026-10-04)

Status: local and S3 byte-source adapters implemented; HTTP/Lua S3 selection integrated. Plan repository
reading for both local files and S3 from the outset. Do not extend the readers
around an assumption that every source must ultimately be a local filesystem path.
The local implementation now uses the shared byte-source boundary, but is not the
permanent storage contract.

### Supported versus recommended sources

Document supported input combinations separately from recommended layouts for
fast delivery. The contract must cover container/layout, compression, channels,
bit depth, color profiles, alpha, orientation, main-image selection and pyramid
interpretation. A supported JPEG may require full decoding; this does not make it
unsupported merely because tiled sources can serve regions more efficiently.
Keep implemented capabilities distinct from the target contract.

Both pyramidal tiled TIFF and JPEG 2000 remain intended repository formats.
Pyramidal tiled TIFF is the provisional default recommendation; JPEG 2000 via Grok
remains an alternative to evaluate, not a second-class or rejected format. JPEG
and PNG remain planned simpler source options. No universal performance winner
has been established.

Compare actual compression/quality settings, not just container names. A TIFF
pyramid halving both dimensions adds approximately one third of the original
pixel count across its overview levels, not necessarily one third of compressed
file size. JPEG 2000 resolution/region access depends on codestream organization
and decoder behavior; define appropriate coding parameters, including access
markers where useful, when evaluating Grok.

For remote TIFF access, use Cloud Optimized GeoTIFF (COG) layout principles as a
reference: accessible directory/offset metadata and deliberately ordered overview
and tile data can reduce remote reads. An arbitrary pyramidal tiled TIFF is not
automatically remote-optimized. We need no geographic metadata for museum images;
adopting these principles does not require declaring every file COG-compliant or
adding GDAL as a runtime dependency.

A future repository validator should distinguish unsupported sources from valid
but less efficient layouts. Share the underlying validation rules with import and
delivery rather than maintaining incompatible definitions in separate programs.

### Reader boundary

The format-independent `ByteSource` boundary is implemented with `info()` and
`read_at(offset, destination)`, plus a RAII `LocalFileSource` using positioned
reads. TIFF metadata/pixels and HTTP probing share one open source. There is no
general storage framework or backend registry. See [byte sources](byte-source.md).

The boundary must express source size, reading at a byte offset, stable identity
and version, ownership/lifetime, concurrency assumptions and read failures.
LocalFileSource and S3ByteSource provide those operations. The latter uses AWS SDK
for C++, pinned versions/conditional ranges and bounded response buffers; see
[S3 byte sources](s3-byte-source.md).
The TIFF adapter translates them to per-handle read/seek/size callbacks through
`TIFFClientOpenExt`, safely propagating storage exceptions after C calls return.
Inspect the selected Grok version's I/O facilities before fixing its adapter.
Grok's advertised S3 support is an option to evaluate, not an assumed implemented
capability of SIPI-2 or a reason to duplicate storage/version rules per codec.

Source resolution and preflight must eventually select a source descriptor that
can represent either storage kind. Keep storage credentials in trusted backend
configuration, outside IIIF URLs and cache keys. The public service identity must
remain independent of whether the source is local or remote. Lua string sources remain local; `{type="s3", key="..."}` selects an object in
the configured bucket. URL prefixes never participate in default source mapping;
project-specific translation belongs in preflight.

### Remote reads, consistency and cache

Read required S3 byte ranges rather than requiring a full-object download for
every crop. One codec read must not imply one network request: use bounded read
buffers, reuse fetched ranges and coalesce nearby reads; control concurrency when
fetching independent ranges. Amazon S3 accepts one range per GET, not multiple
disjoint ranges in a single GET. Avoid promising savings for formats/layouts that
inherently need a large fraction of the source.

All metadata and pixel reads for one rendering operation must refer to one source
version. Prefer explicit VersionId selection when the store supports versioning;
otherwise use conditional reads with a defined change-detection/retry policy.
Treat ETags as opaque validators, not universal content hashes. A replaced source
must never produce mixed-version metadata/pixels or reuse an obsolete cache entry.
Backend identity (including endpoint/bucket/key for S3) and version participate in
cache identity; do not model a remote source as just its public IIIF identifier.

The planned local encoded-result cache and SQLite index remain local even when
the repository uses S3. A source-range cache serves a different purpose and needs
its own bounded accounting; distinguish it from source metadata, reduced-source
derivatives and final encoded responses. This does not call for implementing all
cache layers now.

### MinIO integration tests and format benchmarks

The owner explicitly proposes a small local MinIO S3-compatible store for early
integration testing. Use a configurable endpoint and isolated test buckets/objects
when implementing the S3 adapter; keep production AWS credentials unnecessary for
local tests. No MinIO instance is installed or started by this documentation step.

Verify range reads, version/conditional-read behavior, replacement during a
request, missing objects, access failures and backend error handling. Compare the
same fixture's local and remote metadata/pixel results. Observe requested ranges,
request counts and transferred bytes so an accidental full download is detectable.
MinIO compatibility tests do not replace checks against the intended production
provider, and loopback latency does not represent a remote deployment.

Before final format recommendations, benchmark representative museum images with
matched source data and comparable output fidelity. Include small crops, overview
requests and larger outputs, local and S3-backed storage, and cold/warm result and
source caches. Record source size, transferred bytes, remote request count, CPU
cost and response-time distribution; use realistic network latency as well as
local tests. Do not compare lossy JPEG 2000 to lossless TIFF as equivalent quality.
Keep measured results tied to codec versions and encoding/layout parameters.

Open implementation choices: server configuration and credential
integration, whole-operation deadlines/cancellation, range reuse and budgets,
recommended TIFF/JPEG 2000 encoding profiles and validator interface.

## Agreed processing order

1. Parse the IIIF URL syntactically, without filesystem access.
2. Run optional preflight authorization and source resolution. Determine the
   definitive source and applicable restrictions before reading its header.
3. Check the definitive source version. Reuse matching cached source metadata
   or read its header on a metadata miss. For a restricted variant, establish
   the dimensions of the effective image exposed to the client.
4. Validate and resolve geometry against the effective image and applicable
   policy; determine the concrete rendering operations.
5. Determine the canonical URL and internal cache identity, then check the cache.
6. On a miss, obtain the effective pixels using optimized decoding where possible
   and perform the requested transformations.
7. Encode and deliver the result, optionally writing the encoded output to cache.

A preflight-selected path must never be validated using another file's metadata.
Authorization also applies to cache hits, image information, and HEAD requests.
Preflight failure must not implicitly grant access.

The logical IIIF operation order is region, size, mirroring, rotation, quality,
and format. Decoder optimizations must preserve these semantics. Watermark
placement and its interaction with color/quality conversion remain to be defined.

## Restricted public image variants

The museum use case limits publicly accessible image detail, rather than merely
the pixel count of each response. A per-response size limit is insufficient:
clients could assemble many original-resolution crops into the complete image.

Treat the reduced full image as a distinct effective source. Preserve aspect
ratio when fitting it within the configured bounds, without enlarging a smaller
original. All subsequent IIIF operations use this reduced source.

For example, a 6000 x 4000 original can have a public variant of approximately
1000 x 667 pixels (nearest integer, ties upward, at least one pixel per axis):

- Its info.json reports the reduced dimensions.
- Pixel regions refer to the reduced image's coordinate system.
- Percentage regions also refer to that effective image.
- full/max returns the reduced full image, subject to any additional output limits.
- Combining crops cannot reveal detail absent from the reduced source.

Give the public and original variants separate stable service identities. Example
paths are /iiif/public/image123 and /iiif/original/image123; these are illustrative,
chosen by deployment Lua policy, not hard-coded C++ routes. The original still requires authorization.
Do not silently change a service's coordinate system according to login state.
Separate identities preserve the meaning of links, regions, and annotations.

## Initial implementation strategy for reduced variants

Generate the complete reduced image on first demand and cache it for reuse.
Use a suitable TIFF pyramid or JPEG 2000 resolution level when available, then
resample to the variant's defined dimensions. Every crop comes from the same fixed
reduced pixel raster. Avoid independently reducing arbitrary original crops in a
way that changes sampling alignment between requests.

This is a delivery-time derivative; it does not require an upload pipeline or a
pre-generated derivative for every original. A 1000 x 1000 RGB8 raster occupies
about 3 MB of pixel storage, but aggregate memory/cache usage must be bounded.
The implementation uses a bounded immutable in-memory VariantCache; completed
encoded responses continue to use the persistent result cache. See
[restricted resolution](restricted-resolution.md) for lifecycle and configuration. Source changes and changes to the derivative recipe must
invalidate the corresponding derivative and rendered responses.

Distinguish the cache of reduced sources from the cache of final encoded responses.
They may eventually share storage infrastructure, but have different contents and
identities. No generic cache framework is required at this stage.

## Detail limits versus output limits

The effective source limits available detail. Upscaling it can produce more pixels
without recovering original detail; the existing size-upscaling policy remains a
separate control. Rotation can also increase the enclosing raster dimensions.
Whether a deployment additionally limits the final encoded image's dimensions,
including after rotation, needs an explicit policy.

Do not silently substitute a smaller result for an explicit IIIF size. Adaptive
forms such as max and confined sizes follow their defined limit semantics. Existing geometry errors and published global output limits apply unchanged;
info.json width/height now describe the reduced public canvas.

## Cache architecture (agreed 2026-10-04)

Status: identity, persistent encoded-result storage and authorized HTTP lookup/fill
are implemented. Source metadata is now reused through a bounded worker-local
[in-memory cache](metadata-cache.md); a persistent metadata table is not needed
for this increment. See [cache identity](cache-key.md) and
[persistent storage](persistent-cache.md) for the implemented capacity, locking,
publication and recovery contract. Start with one local cache per SIPI-2
instance: ordinary encoded image files in a configurable persistent directory,
plus a SQLite index. No distributed cache, shared multi-instance directory, or
interchangeable database abstraction is required for the first implementation.
SQLite supports the lookup, size accounting, invalidation and last-use ordering
we need without a separate database service. A key-value store would still need
those management operations to be implemented.

### Stored data and lookup order

Distinguish source metadata from final encoded responses. They can use separate
tables in the same SQLite database; output crop dimensions cannot substitute for
full-source dimensions when resolving percentages, square regions or max size.

- Source metadata is associated with the definitive source identity/version and
  contains dimensions, orientation and other information needed for planning.
- An encoded-response entry contains its cache key, relative file path, byte
  length, media type, output width/height and last-use time. Source/version
  associations must support invalidation. The current result schema and derived
  filenames are described in [persistent storage](persistent-cache.md); explicit
  source-indexed purge remains pending.
- Prefer hash-derived filenames distributed among subdirectories. Define a
  deterministic key serialization and hash before implementation; do not use
  raw request paths as storage paths.

The request order is syntax parsing, preflight/source resolution, source-version
check, matching metadata lookup (or header read), current geometry/policy
validation, and final-response lookup. Only then deliver a hit or render a miss.
This avoids repeated header reads for unchanged sources without bypassing current
policy checks. Authorization applies to every hit, including HEAD and info.json.
A preflight failure must not grant access to cached data.

### Cache identity

The canonical IIIF URL is insufficient as the sole internal key. Include:

- Definitive source identity and source version.
- Effective public/restricted variant identity.
- Normalized rendering operations, including region, size, mirroring, rotation,
  quality and output format.
- Output-affecting encoder/color settings, and watermark identity/version when used.
- An explicit rendering-recipe version, changed when processing semantics change
  (for example, a different resampling algorithm).

Equivalent authorized outputs may share entries. Raw tokens and cookies do not
belong in keys. A change of source, effective variant, watermark or recipe must
prevent reuse of obsolete output; old files can then be reclaimed separately.

### Capacity and eviction

Configure a byte limit and a lower cleanup target (hysteresis). Evict entries in
least-recently-used order until usage, including the incoming entry, fits the
target. For example, 100 GiB maximum and 90 GiB target; these are illustrative,
not selected defaults. Reclaim bytes, not a fixed percentage of the file count.

Account for temporary files and concurrent in-progress writes so generation
cannot silently exceed the intended budget. An individually oversized response
may still be delivered within delivery limits, but is not admitted to the cache.
The implemented admission/headroom policy is defined in [persistent storage](persistent-cache.md),
including database/journal overhead. Active deliveries must remain safe during
eviction; choose an explicit open-handle or entry-lifetime strategy.

Batch last-use updates rather than committing a SQLite write for every hit.
Approximate LRU is sufficient; exact timestamp persistence is not a correctness
requirement. The current store batches every 64 hits and flushes before eviction and on shutdown.

### Persistence, publication and recovery

Preserve the cache across normal stop/start. A future container deployment needs
persistent storage for the cache directory and index. The cache remains entirely
rebuildable; it is never the authoritative image repository.

Publish only complete successful encodings:

1. Write to a unique temporary file on the same filesystem as its final location.
2. Complete encoding, close successfully and atomically rename to the final name.
3. Publish the ready entry in the SQLite index.

Filesystem operations and SQLite do not form one atomic transaction. Recovery
must remove abandoned temporary files, reconcile orphan result files and drop
index entries whose files are missing. Missing/incomplete cached data is a miss,
never a successful partial response. Define crash-durability/flush details during
implementation; preserving every recent cache fill after power loss is not the
same requirement as keeping published entries consistent.

Coordinate concurrent requests for the same missing key so they normally share
one generation rather than repeating expensive image work. Keep database
transactions short and outside decoding/encoding. Cache write/index failures
should, where possible, fall back to uncached delivery instead of failing an
otherwise valid image request.

### Replacing repository images

Allow an image to be replaced under the same logical ID. The intended replacement
contract is to write a complete new file separately and atomically replace the old
path, not modify a file while it is being read.

For local files, initially use file identity, byte size and high-resolution change timestamps as
a practical source-version fingerprint. This is not proof of identical content;
the local implementation captures device/inode, size and nanosecond mtime/ctime
from the opened descriptor. Future cache validation still needs implementation.
Do not hash the complete source on every request. A later managed import pipeline
should assign explicit source versions (and can compute content hashes at import).
A missing or changed definitive source cannot reuse metadata/output from an old
version merely because the public URL is unchanged.

Planning and decoding now use the same opened local file descriptor, including
the initial format probe. Atomic replacement leaves in-flight readers on the old
file while subsequent opens use the replacement. In-place writes are prohibited.
Future S3 reads must enforce the same consistency through versions/conditions.

Replacing content or dimensions can change the meaning of existing IIIF crop URLs
and annotations. Permit stable versioned service identities for permanently
citable images as a future repository capability; a replaceable logical ID does
not by itself provide that guarantee.

### Output boundaries and deferred scope

Sequential encoder output should support bounded forwarding to HTTP and cache.
For encoders requiring seeking, use bounded memory with a temporary-file strategy
for larger results. The current JPEG block sink is a starting point; actual socket
backpressure, cancellation and cache commit coordination are not implemented.
Browser/proxy caching of protected responses remains a separate policy; the
server-side cache does not authorize changing current Cache-Control: no-store.

Reduced effective-source derivatives and final encoded responses have different
contents and identities. They may later share storage infrastructure, but the
first cache increment needs no generic multi-layer cache framework.

## Implementation status and deferred scope

- RasterImage8 owns gray/RGB/straight-alpha pixels and source color declarations.
- Lua exposes JSON, HTTP(S), JWT, configuration, explicit shared caching, source
  overrides, watermarks and reduced public resolution. See [preflight](preflight.md).
- Distinct service prefixes and original authorization are deployment Lua policy;
  default source mapping continues to ignore prefixes.
- The full reduced-source raster, fixed rounding and versioned cache identities
  are implemented; see [restricted resolution](restricted-resolution.md).
- Automatic client discovery/switching and full IIIF Authorization Flow remain
  deferred. The delivery feature set is frozen for validation/documentation/release.

## References

- [IIIF Image API 3.0 terminology](https://iiif.io/api/image/3.0/#12-terminology)
- [IIIF size semantics](https://iiif.io/api/image/3.0/#42-size)
- [IIIF operation order](https://iiif.io/api/image/3.0/#46-order-of-implementation)
- [IIIF canonical URI](https://iiif.io/api/image/3.0/#48-canonical-uri-syntax)
- [IIIF Authorization Flow: tiered access](https://iiif.io/api/auth/2.0/#71-tiered-access)

- [SQLite use cases](https://www.sqlite.org/whentouse.html)
- [COG file organization](https://gdal.org/en/stable/drivers/raster/cog.html#file-format-details)
- [Amazon S3 GetObject: ranges, versions and conditions](https://docs.aws.amazon.com/AmazonS3/latest/API/API_GetObject.html)
- [libtiff custom I/O](https://libtiff.gitlab.io/libtiff/functions/TIFFOpen.html)
- [Grok features and I/O](https://github.com/GrokImageCompression/grok)


## TIFF repository profile and next codec steps (2026-10-04)

Recommended import target: pyramidal tiled Classic TIFF or BigTIFF, unsigned RGB8
or RGBA8, sRGB, top-left, contiguous channels, lossless Deflate or LZW.
Tiled sources without pyramids and strip-based sources are supported. Native
Grayscale8 (MINISBLACK/MINISWHITE) is implemented through PNG/JPEG delivery.
See [strips and grayscale](tiff-strips-and-gray.md). Defer direct 1-bit/16-bit/float,
CMYK/Lab/palette, separate planes and other orientations to import conversion
unless a concrete delivery use case justifies them. Import multi-page documents
as separate repository images. JPEG-compressed TIFF/YCbCr needs explicit color
interpretation support rather than a blanket compression-support claim.

RGB8 PNG output and end-to-end RGBA delivery are now implemented.
For RGBA, recognize explicitly declared straight and premultiplied alpha and
normalize processing semantics. Filtering must account for premultiplied color
values; choose the working color space explicitly. Separate mask IFDs and arbitrary
extra channels belong to import initially. Plan PNG transparency and JPEG
composition against white. Alpha filtering and white composition now use stored
channel encoding; no implicit sRGB or linear-light conversion occurs. A future
color-management increment must define a profile-aware working space explicitly.
See [alpha delivery](alpha-delivery.md) for current semantics and limitations.

## JPEG 2000 integration status (2026-10-04)

Grok 20.4.15 header reading is integrated through ByteSource callbacks for local
and S3 sources. JP2 and raw J2K feed existing info/geometry functions; region and
resolution pixel decoding now feeds the shared JPEG/PNG pipeline. The small project-owned Conan recipe pins
release/submodules and builds the shared CPU core. See
[JPEG 2000 input](jpeg2000-input.md) for lifecycle, limits and build instructions.

Repository TIFF policy: normalize non-top-left orientation during import by
transforming pixels and setting the tag. Likewise convert YCbCr JPEG-in-TIFF and
CMYK during import into the supported repository color models; CMYK conversion
requires an explicit source-profile policy. Delivery does not add these variants.
