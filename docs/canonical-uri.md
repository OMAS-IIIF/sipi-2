# Canonical image URLs

Successful image GET/HEAD responses now include both links in one Link header:

```http
Link: <https://images.example/project/id/full/max/0/default.jpg>;rel="canonical", <http://iiif.io/api/image/3/level2.json>;rel="profile"
```

Images are delivered directly with 200, without a redirect. Information documents
advertise canonicalLinkHeader; their own id already supplies the service identity,
so they retain only the profile link. Failed requests and base redirects do not
receive an image canonical link. Link remains readable through the CORS policy.

## Implementation and inputs

The pure function canonical_image_uri in src/http/ImageUri.* takes the parsed
image request, resolved geometry, effective source dimensions, active size policy,
native-gray fact and configured public base URL. It owns its returned string and
performs no I/O, authorization or pixel decoding. The caller must first resolve and
validate the request. The format-neutral geometry is shared with actual rendering.
service_uri in the same module provides segment encoding for both image links
and info.json. URL segments are encoded independently, including encoded slashes,
percent signs and UTF-8. Prefixes and identifiers remain public identities; Lua
source mappings, filesystem paths and S3 keys cannot appear in the link. Host and
Forwarded headers cannot override public-base-url. Queries are omitted because
this service does not use them as image transformation parameters.

The implementation follows [Image API 3 canonical syntax](https://iiif.io/api/image/3.0/#48-canonical-uri-syntax):

- Use the concrete clipped crop; full coverage becomes full, otherwise pixel coordinates.
- Compare the actual pre-rotation output size with max resolved on that crop under
  the same policy. Equal dimensions become max, even when limits reduce the source.
  Other sizes use explicit width,height. Upscaled geometry uses ^max when it
  matches the configured enlarged maximum, otherwise ^width,height. This pure
  syntax support does not enable upscaling in the renderer.
- Preserve mirroring, normalize 360 to 0, and serialize angles as shortest
  round-tripping fixed decimals without exponent notation or locale dependence.
- Color is the current default rendering, so color becomes default. Gray on a
  native gray source is also an identity and becomes default. Gray conversion of
  color sources and explicit bitonal processing retain their distinct qualities.
- Keep the requested explicit output format. Size always precedes rotation;
  the dimensions of the rotated canvas must not enter the size component.

For a 300 by 200 source without tighter limits:

| Requested suffix | Canonical suffix |
| --- | --- |
| pct:0,0,100,100/pct:50/!090.000/color.jpg | full/150,100/!90/default.jpg |
| square/100,/0/gray.png | 50,0,200,200/100,100/0/gray.png |
| 250,150,100,100/max/360/default.png | 250,150,50,50/max/0/default.png |

## Cache and authorization

The worker computes the URI after source admission and request geometry/budget
validation, before result-cache lookup. Fresh responses and cache hits use the
same response builder. The link is not persisted in the encoded cache: two public
aliases or prefixes may share encoded bytes but must retain their own public URL.
The profile link is appended without replacing the canonical link. No cache key,
recipe or SQLite schema change is needed; encoded pixels are unchanged.

Canonical identity does not grant access and is not an immutable version URL.
Following the link still runs preflight, metadata/version checks and current limits.
Changing source content or policy can change the result; Cache-Control: no-store
continues to apply. Future watermarked or restricted virtual sources must supply
their effective public identity, dimensions and rendering-quality semantics here.

## Verification

ImageUriTests checks known forms, encoding, clipping, square/pct regions, explicit
sizes, limits, upscaling syntax, fractional angles and native gray. Each canonical
URI is parsed and resolved again to verify unchanged geometry and idempotence.
HTTP tests follow canonical links and compare encoded bytes, including repeated
requests/cache hits, aliases, prefixes, configured origins, HEAD, limits and gray.
The focused conformance audit now passes 41/41 probes, including canonical links;
this is not external certification or a substitute for deployed browser testing.
