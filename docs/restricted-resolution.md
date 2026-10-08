# Restricted public resolution

A resolution restriction defines the **whole effective image**, not the dimensions
of each response. Limiting response sizes alone lets a client reconstruct original
detail by combining small original-resolution crops.

## Lua policy and stable URLs

```lua
return {
    decision = "allow",
    -- Optional source override, with the existing local/S3 syntax.
    resolution = { max_width = 1000, max_height = 1000 }
}
```

Both axes are required positive Lua integers, at most 4294967295. Strings, floats,
missing axes, unknown keys, metatables and restrictions on `deny` fail the contract
(`500 preflight_failed`); failures never become unrestricted delivery. Absent/nil
`resolution` preserves ordinary original-source behavior. This field combines with
`source` and `watermark`. No OLDAP-specific rule exists in C++.

Use separate stable service URLs, for example `/public/image-id` and
`/original/image-id`. Lua selects the public resolution by prefix and authorizes
original access. **Do not change dimensions at the same service URL according to
login state**: annotations, pixel regions and an open viewer depend on a stable
coordinate system. All aliases of an original must enforce the same access policy.
SIPI cannot infer project authorization or which aliases point to the same image.
The standard repository mapping still ignores the URL prefix; prefix-based path
translation belongs to Lua. See [the example](../examples/restricted-resolution.lua).
The example denies original access until project-specific authorization is added.
From the repository root, for a source in `data/images/image.tif`:

```sh
./build/debug/sipi-2 --iiif-root data/images --preflight-script examples/restricted-resolution.lua
```

Open `/public/image.tif/info.json` or `/public/image.tif/full/max/0/default.jpg` on
the configured listener. The script selects 1000 × 1000 bounds for every public image.

Changing a public resolution policy changes its coordinate system: publish a new
versioned public service URL when existing annotations/viewers must remain valid.
No automatic variant discovery or IIIF Authorization Flow is introduced.

## Processing and coordinates

1. Evaluate Lua before opening any source or looking up cached pixels/results.
2. Open/version-check the definitive local or S3 source and obtain its metadata.
3. Fit the full source into the policy rectangle, preserving aspect ratio and
   never enlarging. Use the existing IIIF `max` rounding: nearest integer, ties
   upwards, at least one pixel per axis, within the bounding rectangle. A 6000 ×
   4000 source bounded by 1000 × 1000 therefore becomes 1000 × 667.
4. Resolve **all** IIIF region/size operations against those public dimensions.
   `info.json` reports them; base redirects and canonical links retain the public
   service URL. Information requests plan/check budgets without decoding pixels.
5. On an image miss, generate the complete reduced raster using the suitable TIFF
   pyramid/JPEG2000 level and the shared area resampler. JPEG/PNG decode fully,
   within the existing budgets. Cache this fixed raster before any watermark.
6. Apply a required watermark on the public canvas, then crop/resize, mirror,
   rotate, change quality and encode as usual.

All public crops come from the same reduced raster. There is no independent
original-resolution crop path, even with `^` upscaling enabled. Upscaling can
increase pixel count but cannot restore detail absent from the public raster.
Gray/RGB, straight alpha and the source ICC/sRGB declaration are preserved.

Global `maxWidth`, `maxHeight` and `maxArea` remain **output** limits, advertised
independently in `info.json`. They do not silently alter the public canvas.
`full/max` can be smaller than that canvas when these output limits require it.
Explicit oversized IIIF sizes still fail normally; they are not silently reduced.
If the complete public raster cannot be generated within the renderer's existing
budgets, source admission fails with `501 source_not_deliverable` for image/info/
base requests. Large policy bounds are not a bypass of processing limits.

## Two distinct caches

`VariantCache` retains immutable complete reduced rasters in memory, shared by the
worker pool. Its identity includes the freshly opened source identity/version/
length, full reduction geometry, pixel layout and a versioned reduction recipe.
Watermarks and tokens do not enter this source cache. Equal authorized public
variants can reuse pixels; each request still evaluates policy first.

| CLI / TOML | Environment | Default |
| --- | --- | --- |
| `variant-cache-max-entries` | `SIPI_VARIANT_CACHE_MAX_ENTRIES` | 16 |
| `variant-cache-max-bytes` | `SIPI_VARIANT_CACHE_MAX_BYTES` | 67108864 (64 MiB) |

Priority: CLI > environment > TOML > defaults. Either zero disables retention,
not restrictions. Eviction is LRU. Accounted bytes include raster/profile vector
capacities, keys and entry storage; allocator/control-block overhead and active
references surviving eviction are additional. Rendering owns a pixel copy; its
working budget is separate from resident cache limits. Limits reserve no memory.
Same-variant cold fills serialize; unrelated images remain parallel. Failed
materialization is never cached. If retention is disabled or admission fails, the
complete reduced raster is still generated before cropping.

The **persistent result cache** continues to store completed JPEG/PNG/WebP
responses. Restricted results use a separate `restricted-v1:WIDTHxHEIGHT` identity,
plus source version, request geometry, output settings and watermark identity.
Original results cannot satisfy a restricted request. A result hit avoids reduced
source generation. Public aliases with identical authorized content can share
bytes while receiving their own freshly generated canonical headers.

After restart the resident raster cache is empty, but valid encoded results remain
available. There is no separate persistent derivative directory/database. Source
replacement invalidates both kinds of cached content through existing version
keys; deleting/unavailable sources cannot be served from warm caches. Local writers
must publish atomically, never modify files in place. S3 uses the existing fresh
HEAD and version-bound read contract. Old versions age out through ordinary eviction.

## Verification and feature freeze

Coverage includes strict Lua contracts, configuration precedence/disablement,
LRU budgets, a separately calculated pixel-area oracle, crop reconstruction,
public/original denial with warm caches, source replacement/deletion, concurrent
crops, watermark/alpha/gray delivery, output-limit independence, upscaling,
local/MinIO equivalence and restart behavior.

This completes the agreed delivery feature set for the first release. The project
now enters feature freeze: correctness/security fixes, regression/conformance
checks, documentation consolidation and release preparation remain in scope.
New formats, upload, additional cache optimization and optional capability discovery
are deferred unless the owner explicitly reopens feature development.
