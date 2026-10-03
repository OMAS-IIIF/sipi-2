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
RGB8 TIFF crop decoder retains source ICC bytes without converting to sRGB;
upload/import, original preservation/download, and HTTP pixel delivery are pending.
Keep RgbImage8 until concrete additional channel/sample requirements justify a
broader representation.

## Agreed processing order

1. Parse the IIIF URL syntactically, without filesystem access.
2. Run optional preflight authorization and source resolution. Determine the
   definitive source and applicable restrictions before reading its header.
3. Read metadata from that source. For a restricted variant, establish the
   dimensions of the effective image exposed to the client.
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
1000 x 667 pixels (the exact rounding contract must be defined in implementation):

- Its info.json reports the reduced dimensions.
- Pixel regions refer to the reduced image's coordinate system.
- Percentage regions also refer to that effective image.
- full/max returns the reduced full image, subject to any additional output limits.
- Combining crops cannot reveal detail absent from the reduced source.

Give the public and original variants separate stable service identities. Example
paths are /iiif/public/image123 and /iiif/original/image123; these are illustrative,
not a finalized route convention. The original still requires authorization.
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
Storage format, persistence, eviction, concurrent generation, and invalidation
are still open. Source changes and changes to the derivative recipe must invalidate
the corresponding derivative and rendered responses.

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
forms such as max and confined sizes follow their defined limit semantics. Exact
HTTP error mapping and publication of effective limits in info.json remain to be
specified when integrating delivery.

## Cache and output boundaries

The canonical URL is not sufficient as the sole internal cache key. Account for
the source identity/version, effective variant, normalized rendering operations,
watermark identity/version, and output-affecting encoder/color settings. Access
must be checked before reusing an entry; equivalent authorized outputs may share
entries without embedding raw tokens in cache keys.

Sequential encoder output should support bounded forwarding to HTTP and cache.
For encoders requiring seeking, use bounded memory with a temporary-file strategy
for larger results. Publish a cache entry only after successful complete encoding.
Actual streaming support depends on the encoder/container and HTTP integration.
Browser/proxy cache policy for protected content is a separate unresolved concern.

## Proposed building blocks and remaining decisions

- A simple RenderPlan value could carry resolved operations, keeping URL and
  preflight interpretation out of codecs. Its interface is not yet designed.
- RasterImage is a candidate name for a future owning pixel representation.
  Keep the existing RgbImage8 until concrete additional sample/channel needs arise.
- Lua 5.4.8 now implements the initial standalone allow/deny and optional-source
  contract; see [preflight](preflight.md). Host-provided JWT verification/signing,
  HTTPS, JSON, and restriction fields are still pending.
- Extend the typed allow/deny decision with the agreed variant/restriction model. Define
  timeouts for future native/network operations. Root mapping is implemented in
  SourceResolver; relative Lua source results now use iiif-root. The initial
  HTTP integration uses isolated Lua states, memory/instruction budgets, and a
  bounded worker; blocking work must not occupy Drogon's I/O thread.
- Define stable variant naming, the restricted-source recipe, rounding, and how
  clients discover/switch between public and authorized services.
- Full IIIF Authorization Flow integration is deferred. Its substitute-resource
  mechanism is compatible with the chosen distinct-variant model.

## References

- [IIIF Image API 3.0 terminology](https://iiif.io/api/image/3.0/#12-terminology)
- [IIIF size semantics](https://iiif.io/api/image/3.0/#42-size)
- [IIIF operation order](https://iiif.io/api/image/3.0/#46-order-of-implementation)
- [IIIF canonical URI](https://iiif.io/api/image/3.0/#48-canonical-uri-syntax)
- [IIIF Authorization Flow: tiered access](https://iiif.io/api/auth/2.0/#71-tiered-access)
