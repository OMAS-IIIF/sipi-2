# Encoded-result cache identity

Step 1 is implemented in `src/cache/CacheKey.h/.cpp`: an owning rendering
identity, deterministic serialization and SHA-256. Persistent storage is now
implemented separately in [ResultCache](persistent-cache.md); the IIIF worker now
uses it after authorization, source-version and request validation. The key describes encoded image bytes, not an HTTP
response or an authorization decision.

## Inputs and normalization

Construct `RenderIdentity` after preflight, definitive source opening, metadata
resolution, geometry/policy validation and renderer capability checks.

| Input | Meaning |
|---|---|
| `source` | Opaque backend-qualified identity, version and byte length from the opened `ByteSource`. |
| `variant` | Required stable identity of the effective image/policy variant; `original` for unrestricted content. |
| `geometry` | Resolved integer crop and output dimensions before mirroring/rotation. Coordinates belong to the effective image. |
| `rotation` | Mirroring flag and clockwise angle; 360 and either signed zero normalize to 0. |
| `quality`, `input_layout` | Resolve default/color to color for RGB/RGBA, or gray for native grayscale. Explicit gray and bitonal remain distinct operations. |
| `encoding` | JPEG with quality (1..100) and effective chroma sampling, PNG, or WebP with lossless/quality and a codec-specific recipe. These types identify the output format. |
| `watermark` | Optional WatermarkIdentity: definitive source identity/version/length, relative width, opacity and sRGB8 color. |
| `recipe` | Version of fixed rendering semantics and encoding/color decisions. Defaults to `current_render_recipe`. |

The same source and variant can share a result for these equivalent requests on
a 1000 x 800 image:

```text
/id/full/500,/0/default.jpg
/id/0,0,1000,800/500,400/360/color.jpg
/another-prefix/alias/pct:0,0,100,100/pct:50/0/default.jpg
```

The last alias shares only if source resolution and current policy produce the
same definitive source/version/variant. URL prefix, identifier, scheme, host,
query credentials and GET/HEAD method are deliberately absent. HEAD can use the
same encoded representation later; request-specific headers remain separate.
The key does not canonicalize a public IIIF URL.

Percentage rounding, square placement, clipping, size limits and `^` permissions
are resolved before key construction. A denied request cannot become authorized
because its geometry would match an existing key. Do not infer additional pixel
equivalence, such as a symmetric image after rotation or a gray-looking RGB image.
Exact unequal angles remain unequal: no rounding tolerance is introduced.

## Codec migration

Recipe v4 replaces IJG libjpeg 9f with libjpeg-turbo 3.2.0 in both reading and
writing (including libtiff). It intentionally invalidates prior encoded results
for every format: a JPEG source can also change a PNG/WebP result. The old cache
schema is unchanged and obsolete entries expire through normal LRU cleanup. The
same policy separates in-memory reduced-source variants; no per-codec key machinery
is needed. Subsequent quality/sampling changes still use their explicit fields.

## Variants and versioning

A future reduced virtual source must use a different `variant` from the original,
even when crop/output numbers happen to coincide. Its identity must cover virtual
image dimensions and all output-affecting policy parameters. Watermark content
is versioned separately; position/opacity/composition policy also belongs to the
effective variant or recipe. Raw tokens, cookies and user IDs are not variants.
Equivalent authorized outcomes may share an entry regardless of the user.

Preflight currently implements allow/deny and source overrides, not restrictions.
Before restrictions are integrated, their authoritative representation must
produce this stable variant identity from the actual effective parameters.
CacheKey does not invent policy semantics or turn an arbitrary role label into
a safe variant. The opaque identity is an explicit contract with that future
policy implementation, not a substitute for implementing it.

`current_render_recipe = "sipi-render-v4"` covers current TIFF/Grok resolution
planning, stored-sample area filtering, preserved ICC, straight alpha, encoded
white JPEG composition, baseline JPEG/4:4:4 and PNG compression level 6 without
interlacing. JPEG quality is an explicit field. The shared `JpegOptions` value
is used for both encoding and identity. Color 4:2:0 adds `jpeg.sampling=4:2:0`;
absence means the historical 4:4:4 recipe, preserving all existing default keys.
Native gray, explicit gray and bitonal outputs ignore sampling because they have
one component. Quality still affects their keys. PNG/WebP and unencoded reduced
source variants do not acquire JPEG settings. Changing only these settings needs no recipe bump or cache purge; returning
to previous settings within the same codec recipe can reuse retained entries.
Bump the recipe when fixed algorithms, color treatment, settings or codec changes
can affect bytes. Do not use the application display version as a substitute.
When another setting becomes configurable (e.g. target color profile), add its
actual value/identity to the descriptor and version the serialization accordingly.
Integration must use the same resolved settings as rendering, not independent
copies of encoder defaults.

## Serialization and hash contract

Serialization starts with `sipi-result-key-v1\n`. Fixed named fields follow in a
fixed order, each encoded as:

```text
name:<decimal byte length>:<exact value bytes>\n
```

Strings are opaque bytes: colons, newlines, UTF-8 and embedded NUL do not blur field
boundaries. Integer conversion uses `std::to_chars`, independent of the locale.
Angles use normalized IEEE-754 binary64 bits in 16 lowercase hexadecimal digits;
this preserves the parsed double exactly and avoids decimal-library differences.
No host byte-order dump, struct padding, `std::hash` or JSON formatting is involved.
The key schema version and rendering recipe version serve different purposes.

OpenSSL EVP computes SHA-256 over these bytes, yielding 64 lowercase hex digits.
OpenSSL 3.6.5 was already pinned transitively; it is now an explicit Conan
requirement, privately linked through `OpenSSL::Crypto` by `cachekey`.

The persistent index retains serialization alongside the digest and compares it
on a hit as a collision safeguard. A digest collision with unequal serialization
must not serve or overwrite the other entry; fall back to uncached delivery.
Digest-derived paths and sharding belong to ResultCache. Neither the
serialization (which may contain private source identifiers) nor authorization
material should be exposed in HTTP/cache logs. The digest itself is not a secret
or an access credential.

## API example

For an already authorized, validated RGB-to-JPEG request:

```cpp
sipi::cache::RenderIdentity identity{
    .source = source.info(),
    .variant = "original",
    .geometry = geometry,
    .rotation = request.rotation,
    .quality = request.quality,
    .input_layout = sipi::PixelLayout::rgb,
    .encoding = sipi::cache::JpegEncoding{90},
    .watermark = std::nullopt
};
const sipi::cache::CacheKey key(identity);
// key.serialized(): owning key's immutable identity representation.
// key.digest(): 64 lowercase hexadecimal characters.
```

CacheKey owns its strings; accessor views remain valid while that value is alive
and unchanged. Input values can be discarded after construction. Independent
construction is thread-safe. Missing source/version/size, variant/recipe, invalid
geometry/enum/angle or JPEG quality throws `std::invalid_argument`. Structural
validation cannot check image bounds, policy approval or actual codec support.
Hash errors throw `std::runtime_error`; allocation failures may propagate.

Tests cover parser/geometry equivalence, clipped/square/percentage requests,
native grayscale, separation of output-affecting inputs, watermark replacement,
binary-safe framing, ownership, locale independence, concurrent hashing and
invalid input. A frozen serialization plus independently computed SHA-256 golden
vector protects the persistence contract across builds.


The next increment is now implemented as [ResultCache](persistent-cache.md):
persistent encoded files, a SQLite index, bounded admission and restart recovery.
HTTP lookup/fill lives in IiifWorker, separate from identity and storage.

## Orthogonal transforms

Geometry in the key remains the resolved crop and **pre-rotation** size. The
existing mirroring/angle fields identify the transform; 0/360 remain equivalent.
The persistent result metadata instead records final encoded dimensions, swapping
width/height at 90/270 degrees. The rendering recipe remains unchanged for this
addition: previously supported identity output is byte-for-byte unchanged, and
unsupported mirrored/rotated requests never populated the result cache.

Rendering recipe v2 introduced LCMS gray conversion and native PNG
gray-alpha preservation. The serialization schema remains v1; prior v1 recipe
entries no longer match and are evicted normally.


For `bitonal` with `floyd-steinberg`, serialization adds the named
`bitonal.method` field. Its absence means the original threshold renderer.
Other qualities ignore the setting and preserve their keys. This additive v1
extension preserved existing method identities. The fixed recipe is now v3
because bitonal PNG uses packed 1-bit encoding.
Future algorithm changes still require a recipe increment.


Rendering recipe `sipi-render-v3` invalidates prior encoded results to guarantee
one-bit `bitonal.png` on cache hits as well as misses. This intentionally versions
the whole renderer, so other-quality entries are regenerated too. No SQLite schema
migration or cache directory deletion is necessary; old entries age out normally.

WebP output adds independent encoding fields and a codec-specific recipe; JPEG/PNG
serialization is unchanged. See [WebP cache and color policy](webp-delivery.md).

Single-color watermark delivery adds a `watermarked` variant and complete typed
watermark parameters. Width/opacity preserve exact binary64 values (signed zero
normalized); a fixed `center-fit-direct-area-linear-srgb-bpc-v1` subrecipe versions
placement, sampling and color semantics. Unmarked schema-v1 serialization is
unchanged. Both source validation and mask decoding precede lookup; see
[watermark delivery](watermarks.md#http-delivery-and-cache-identity).

## Restricted public variants

Restricted results use `restricted-v1:WIDTHxHEIGHT`, covering the effective full public canvas independently of crop geometry. The source version, all output settings and optional watermark identity remain part of the key. Complete reduced rasters use a separate `restricted-source-v1` key in the memory-only VariantCache. See [restricted resolution](restricted-resolution.md).
