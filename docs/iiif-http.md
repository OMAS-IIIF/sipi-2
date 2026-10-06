# IIIF HTTP behavior

The service publishes Image API 3 level2 for sources admitted by the shared
[delivery policy](output-limits.md). JPEG/PNG, required image operations and
per-resource qualities are implemented; this is not external certification.

## Routing and discovery

GET/HEAD on a base URI uses the same Lua authorization, source selection and
metadata admission as info.json, then returns 303 with Location pointing to
that resource's info.json. Missing/denied/unsupported sources retain their errors.
The public base URL comes only from configuration; encoded prefix segments and
identifier are retained. Source paths never become public service identifiers.

Exact health and hello routes take precedence. The strict core parser still
accepts image/info requests only; HttpProtocol classifies base URIs separately.
With arbitrary prefixes, base and malformed image paths can overlap. Valid
image/info syntax wins. A path with at least five segments is reserved as an
image request when its region position looks like full/square/pct:/coordinates,
or its final component looks like default/color/gray/bitonal plus an extension.
Errors on those paths remain 400 instead of redirecting to another identity.
Other well-formed paths are base URIs. Root, trailing slash and malformed escapes
remain errors. Repository identifier mapping continues to exclude the URL prefix.

Successful image and info responses include a level2 profile Link header, including
encoded-cache hits and HEAD responses. Image responses also include [canonical links](canonical-uri.md), recomputed for
the public identity on each request; internal cache keys are separate. Unsupported output
formats return 400. Worker responses retain Cache-Control: no-store.

## Information content negotiation

Absent Accept and equal preferences select application/ld+json with the IIIF
context profile parameter. application/json is also available. Selection honors
wildcards, q-values, specificity, UTF-8 charset and the IIIF context profile;
explicit q=0 exclusions override less-specific wildcards. Unsupported parameters
or invalid ranges do not match. No acceptable representation returns 406.
Information success and negotiation errors carry Vary: Accept. HEAD has identical
headers to GET without a body. Errors remain ordinary JSON.

## Configurable CORS

Defaults are cors-origins="*" and cors-credentials=false: public browser access,
including explicit Authorization headers, without cross-origin cookies.
For cookie-based viewers use an explicit comma-separated list and credentials:

```toml
cors-origins = "https://viewer.example,https://catalog.example"
cors-credentials = true
```

CLI --cors-origins/--cors-credentials and SIPI_CORS_ORIGINS/SIPI_CORS_CREDENTIALS
follow CLI > environment > TOML > defaults independently. The origins setting
is one string in every source, not a TOML array. No whitespace, paths, wildcards
inside hostnames, credentials, query, fragment, opaque null origin or empty entry
is allowed. Use browser-serialized origins (lowercase host, no default port).
Explicit origins are matched byte-for-byte. A list without credentials is valid.
Wildcard plus credentials is rejected at startup; no unrestricted reflection.

All application responses, including errors/redirects, receive wildcard CORS or
the matching allowed origin. Only matches in credential mode receive
Access-Control-Allow-Credentials: true. List mode adds Vary: Origin, preserving
Vary: Accept, even for missing/disallowed origins. Link and Location are exposed.
Unlisted origins receive no CORS grant; this does not prevent non-browser access.

OPTIONS advertises GET/HEAD/OPTIONS and Authorization/Accept/Content-Type headers
without executing Lua or reading a source. Unsupported requested methods/headers
return 403. An unlisted origin receives no CORS grant on OPTIONS either. Actual
GET/HEAD requests always execute authorization, including encoded-cache hits.
Other methods return 405 with Allow. CORS is not authentication or CSRF protection.
The app does not issue cookies: login infrastructure must set appropriate cookie
attributes, HTTPS and viewer credentials options; browser third-party-cookie
restrictions still apply. Avoid duplicate/conflicting CORS headers in Caddy.

## Verification

HttpProtocolTests covers path classification and Accept grammar. The HTTP suite
covers GET/HEAD, redirects, errors, cache hits, wildcard/list CORS with and without
credentials, allowed/disallowed/null origins, OPTIONS, profile and qualities.
CLI tests cover syntax and priority across all sources. Actual Caddy/browser and
external-validator interoperability remain deployment checks.
