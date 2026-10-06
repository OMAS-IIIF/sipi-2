# Image information

GET `/{prefix...}/{identifier}/info.json` authorizes the request, resolves the
definitive source, reads its TIFF, JPEG 2000, JPEG or PNG metadata, and returns status 200 with an
JSON-LD document by default (JSON when selected by Accept). HEAD performs the same policy and metadata checks,
returning the same headers without the body. All worker responses, including
successful information documents, carry `Cache-Control: no-store`.

For the generated 320 x 200 test image and default listener:

```json
{
  "@context": "http://iiif.io/api/image/3/context.json",
  "id": "http://127.0.0.1:8080/project/sample.tif",
  "type": "ImageService3",
  "profile": "level2",
  "protocol": "http://iiif.io/api/image",
  "width": 320,
  "height": 200,
  "maxWidth": 8192,
  "maxHeight": 8192,
  "maxArea": 8388608,
  "extraFeatures": ["canonicalLinkHeader", "mirroring", "profileLinkHeader", "rotationArbitrary"],
  "extraQualities": ["color", "gray", "bitonal"]
}
```

See the [2026-10-06 conformance audit](iiif-conformance.md) for verified gaps and implementation order.

## Published capabilities

Admitted resources now publish `profile: level2`. This retires the earlier,
owner-approved omission after resource admission and required HTTP behavior were
implemented. It is an implementation declaration, not external certification.
The focused audit passes all 41 probes, including optional canonical links.
See [HTTP behavior](iiif-http.md) and [audit evidence](iiif-conformance.md).

Level 2 supplies JPEG/PNG, default quality, required region/size forms and quarter
turns. Additional features are mirroring, arbitrary rotation, canonical links and profile links.
Color sources add color/gray/bitonal qualities; native gray adds gray/bitonal.
No sizes, tiles, preferredFormats or extraFormats are emitted. Upscaling remains
unavailable in rendering and is not advertised, even if geometry permits it.
Unsupported layouts and predictably unaffordable full/max sources return 501 on
both info and images. Admission does not decode pixels or prove file integrity.

## Dimensions and source identity

`src/http/IiifInfo.*` takes a decoded Resource, width/height, and the public base
URL. It knows neither TIFF nor filesystem paths; it returns an owning JSON value.
The worker supplies dimensions only after delivery admission. Internal TIFF
metadata retains orientation-normalized dimensions, but orientation 6 currently
returns 501 at the public info endpoint because its pixels are unsupported.
Classic TIFF and BigTIFF follow the same path. No pixels are decoded.

Lua-selected sources determine dimensions, but never replace the public identifier
with a local path. For example, mapping `public-id` to `large.tif` reports that
file's dimensions while keeping `/public-id` in `id`. Restricted virtual sources
are not implemented; later their effective dimensions and separate service
identity must be selected before building this document.

## Public URL and encoding

The base is the validated `public-base-url` setting or `http://127.0.0.1:<port>`.
Each decoded prefix segment and the identifier is separately percent-encoded and
appended. Encoded slashes remain inside their original segment, percent signs are
escaped once, plus is literal data, and UTF-8 bytes become percent escapes.
The result has neither `/info.json` nor a query string. Examples:

| Request path | Service path in id |
| --- | --- |
| `/museum/subpart/id/info.json` | `/museum/subpart/id` |
| `/project%2fpart/a%2fb/info.json` | `/project%2Fpart/a%2Fb` |
| `/a%252Fb/info.json` | `/a%252Fb` |
| `/a+b/info.json` | `/a%2Bb` |

Dot-only resource segments (`.` or `..`, also encoded) are rejected by the parser;
otherwise client normalization could change the service identity.

Configure the public HTTPS origin explicitly behind Caddy. Host, Forwarded and
X-Forwarded-* headers cannot influence this value. Include a mount path only when
the proxy strips it before forwarding; otherwise the parsed prefix already
contains it. See [configuration](command-line.md#public-service-address).

## Validation

The HTTP tests use generated real TIFFs and check the complete document, oriented
dimensions, BigTIFF, Lua source overrides, prefixes, escaping, hostile Host/proxy
headers, configured and default origins, GET/HEAD parity, no-store, and unchanged
deny/missing/corrupt-source handling. Configuration tests cover precedence,
trailing-slash normalization, IPv6, malformed URLs and help/version boundaries.

JPEG 2000 JP2/raw J2K also supplies information, geometry checks and supported
8-bit gray/RGB region/resolution pixel delivery through the same local/S3 source;
see [JPEG 2000 input](jpeg2000-input.md).

JPEG headers now participate in the same information path for accepted 8-bit
gray/RGB sources with absent/Top-left EXIF orientation. They advertise mirroring
and rotationArbitrary (quarter turns are included in level2), plus gray and bitonal qualities for supported pixel sources.
Other JPEG and TIFF orientations are rejected for both public info and delivery. See [JPEG input](jpeg-input.md).


PNG headers supply IHDR dimensions through the same metadata path. Pixel-supported
sources advertise mirroring/rotationArbitrary (quarter turns are included in level2), plus gray and bitonal qualities.
Color encodings the current raster cannot preserve return 501 on public info and delivery. See
[PNG input](png-input.md) for the complete supported subset and validation boundaries.

Successful info reports maxWidth/maxHeight/maxArea from the same policy used by requests; see [output limits](output-limits.md).
