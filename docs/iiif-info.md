# Image information

GET `/{prefix...}/{identifier}/info.json` authorizes the request, resolves the
definitive source, reads its TIFF, JPEG 2000 or JPEG metadata, and returns status 200 with an
`application/json` document. HEAD performs the same policy and metadata checks,
returning the same headers without the body. All worker responses, including
successful information documents, carry `Cache-Control: no-store`.

For the generated 320 x 200 test image and default listener:

```json
{
  "@context": "http://iiif.io/api/image/3/context.json",
  "id": "http://127.0.0.1:8080/project/sample.tif",
  "type": "ImageService3",
  "protocol": "http://iiif.io/api/image",
  "width": 320,
  "height": 200,
  "extraFeatures": ["mirroring", "rotationBy90s", "rotationArbitrary"]
}
```

## Explicit development boundary

The owner approved leaving `profile` out until pixel delivery supports a genuine
compliance level. This document is therefore **not yet fully IIIF 3 conformant**:
the [Image API](https://iiif.io/api/image/3.0/#52-technical-properties) requires
`profile` to name the highest fully supported level, and even level0 needs image
output. Add the appropriate profile when its actual requirements are implemented
and tested, rather than declaring the eventual target now.

No `sizes`, `tiles`, `preferredFormats` or `extraFormats` are emitted yet.
Sources with a supported pixel layout advertise
`extraFeatures: ["mirroring", "rotationBy90s", "rotationArbitrary"]`; metadata-only layouts do not.
See [orthogonal transforms](orthogonal-transforms.md). Pixel-readable sources advertise `extraQualities: ["gray", "bitonal"]`; RGB conversion uses
LittleCMS and native grayscale remains unchanged (see [color management](color-management.md)). Parsing a parameter, recognizing an input codec,
or reading a TIFF pyramid does not mean the server can render its output. In
particular, `size-upscaling=true` does not advertise `sizeUpscaling` prematurely.
Content negotiation/JSON-LD media type, CORS and base-URI redirects are separate
future steps. For now the response always uses application/json, as do our other
JSON endpoints. Supported tiled or strip-based gray/RGB/RGBA8 TIFF crops now return JPEG or PNG; unsupported
operations/layouts return 501, except unsupported rotation angles, which return 400. Profile publication remains pending a per-resource
compliance review: metadata-readable sources are not all pixel-deliverable yet.
See [JPEG delivery](jpeg-delivery.md).

## Dimensions and source identity

`src/http/IiifInfo.*` takes a decoded Resource, width/height, and the public base
URL. It knows neither TIFF nor filesystem paths; it returns an owning JSON value.
The worker supplies orientation-normalized display dimensions. A TIFF stored as
320 x 200 with orientation 6 is reported as 200 x 320, matching geometry validation.
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
rotationBy90s and rotationArbitrary, plus gray and bitonal qualities for supported pixel sources.
Other JPEG orientations are rejected, unlike the metadata-only TIFF orientation
reporting described above. See [JPEG input](jpeg-input.md).


PNG headers supply IHDR dimensions through the same metadata path. Pixel-supported
sources advertise mirroring/rotationBy90s/rotationArbitrary, plus gray and bitonal qualities.
Color encodings the current raster cannot preserve remain metadata-only. See
[PNG input](png-input.md) for the complete supported subset and validation boundaries.
