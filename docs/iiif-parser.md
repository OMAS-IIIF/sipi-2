# IIIF syntax parser

`src/iiifparser/IIIFParser.h` exposes `sipi::iiif::parse_path` for the
[IIIF Image API 3.0](https://iiif.io/api/image/3.0/). It is a standalone C++20
library with only standard-library dependencies. The HTTP catch-all calls it using
the original encoded path; see [HTTP behavior](http-server.md).

## Input and result

Pass the **original, percent-encoded absolute path**, without a query or fragment.
For a future Drogon adapter, use `getOriginalPath()`, not an already decoded path.
The parser accepts:

```text
[/prefix]/identifier/region/size/rotation/quality.format
[/prefix]/identifier/info.json
```

The leading slash is mandatory; the prefix can have any number of segments.
For example, `/myproj/subpart/imageid/full/max/0/default.jpg` produces a prefix
of `{"myproj", "subpart"}`, identifier `"imageid"`, full region, maximum size
without upscaling, zero rotation without mirroring, default quality, and JPEG.
For image requests, the last four segments are parameters and the preceding
segment is the identifier. For information requests, the final segment is
`info.json` and the preceding segment is the identifier.

The return type is `std::variant<ImageRequest, InfoRequest, ParseError>`:

```cpp
const auto result = sipi::iiif::parse_path("/collection/id/full/max/0/default.jpg");
if (const auto* image = std::get_if<sipi::iiif::ImageRequest>(&result)) {
    // image->resource.identifier owns "id".
    // Inspect image->region and image->size.value using std::get_if/std::visit.
} else if (const auto* error = std::get_if<sipi::iiif::ParseError>(&result)) {
    // error->component identifies the invalid parameter; message explains it.
} else {
    // Information request: std::get<sipi::iiif::InfoRequest>(result).
}
```

All result data is owned by value. There are no retained views, manual allocations,
custom copy/move operations, shared mutable state, or I/O. Syntax failures return
`ParseError`; allocation failures follow ordinary standard-library behavior.

## Supported parameters

| Component | Syntax |
| --- | --- |
| Region | `full`, `square`, `x,y,w,h`, `pct:x,y,w,h` |
| Size | `max`, `w,`, `,h`, `w,h`, `!w,h`, `pct:n`; each optionally prefixed with `^` |
| Rotation | Decimal from 0 through 360, optionally prefixed with `!` for horizontal mirroring |
| Quality | `default`, `color`, `gray`, `bitonal` |
| Format | `jpg`, `tif`, `png`, `gif`, `jp2`, `pdf`, `webp` |

Region and size alternatives use separate structs inside `std::variant` so that,
for example, a width-only request does not need a dummy height or an extra mode flag.
`Size::allow_upscaling` records `^`; `Rotation::mirrored` records `!`.
Format recognition only records a requested format, not actual codec availability.
Unlisted extensions are currently rejected; adding one requires an explicit mapping.

Pixel coordinates and dimensions are unsigned 32-bit integers; overflow is rejected.
Dimensions must be positive, while region origins may be zero. Percentages and
rotation use finite, representable `double` values. Decimal syntax permits digits
and one dot, with at least one digit. `.5`, `1.`, and `01.00` are accepted as
noncanonical spellings; signs, exponent notation, whitespace, NaN, infinity, and
numeric suffix garbage are rejected. Decimal underflow reported by `from_chars`
is also rejected. Percentage sizes must be positive and cannot exceed 100 without
`^`. All keywords and format names are case-sensitive.

Region percentages may exceed 100: clipping and detecting a region outside the
image belong to geometry resolution. Pixel regions are not added or multiplied
here, so syntax parsing cannot overflow while calculating boundaries or areas.

## URL decoding and boundaries

Split on literal `/` **before** percent decoding. Each component is decoded once:

- `/a%2Fb/info.json` has identifier `a/b`.
- `/a%252Fb/info.json` has identifier `a%2Fb`, not `a/b`.
- `/a+b/info.json` has identifier `a+b`, not `a b`.
- Prefix segments also retain their original boundaries in a vector.

Malformed escapes and empty segments are rejected. Prefix/identifier segments
that decode to exactly `.` or `..` are rejected because clients normalize them,
preventing a stable service identity. As an application policy,
ASCII control bytes, including NUL and DEL, are rejected, both literal and encoded.
Other bytes are preserved; this parser does not normalize or validate Unicode.
Decoded names are logical resource names, **not filesystem paths**. A later resolver
must define collection access and filesystem containment; it must not decode again.

## Deliberately separate steps

The syntax parser does not calculate pixel geometry, clip regions, preserve aspect ratios,
round output sizes, enforce image/server limits, choose decoder reduction levels,
construct canonical URLs, or open images. Those operations need image metadata or
server policy. Legacy `red:` sizes, size `full`, page suffix handling, and silent
quality/format defaults are not carried over from cserve.

Bare resource URLs and their redirects are not supported by this entry point.
With an arbitrary prefix, a short malformed image path is indistinguishable from
some bare resource names. Base-URI handling therefore needs an explicit routing or
resource-resolution policy instead of silently treating failed image parsing as a
successful base request.

HTTP routing reserves `/health` and `/hello` before handing other paths to IIIF.
Future application endpoints such as `/upload` must also take precedence. This parser itself has no routing or
HTTP-method responsibilities.

## Verification

`tests/IIIFParserTests.cpp` checks typed values for every parameter alternative,
quality/format mappings, prefixes, information requests, decoding exactly once,
input lifetime, numeric boundaries, and malformed inputs. Run through CTest:

```sh
cmake --build --preset conan-debug
ctest --preset conan-debug --output-on-failure
```

Image-dependent region/size resolution now lives separately in `IIIFGeometry.h`
and `.cpp` in the same module. See [TIFF metadata and geometry](tiff-metadata-and-geometry.md).
