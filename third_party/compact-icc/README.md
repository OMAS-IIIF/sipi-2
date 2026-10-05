# Embedded sRGB profiles

Source: [Compact ICC Profiles](https://github.com/saucecontrol/Compact-ICC-Profiles)
by Clinton Ingram, revision `bdd84663061bc4ae95ca70decff54f581e27f702`.
License: CC0-1.0; complete text in LICENSE.

| File | Bytes | SHA-256 |
| --- | ---: | --- |
| sRGB-v4.icc | 480 | c56e1685d888f5edb92fe07f2750f387f8fe8e91b32ff8fb0b56bfbbb9458353 |
| sGrey-v4.icc | 360 | 00c0f94e09127520a17dc0e1d9264b5702081d96dbdb1549ee88e3631ce42a9d |

The gray profile uses the same transfer curve and white point as sRGB. ICC v4
parametric curves avoid a sampled-curve approximation. These are actual usable
profiles, unlike the deliberately minimal headers in tests/IccFixture.h.

src/image/SrgbProfiles.cpp embeds verbatim byte arrays from these files. No runtime
file location or download is needed. When emitting a JPEG, srgb_profile copies the
matching array, sets the rendering intent at bytes 64..67 and clears the optional
profile ID at bytes 84..99. Nothing else is changed. tests/png_http.py compares every
emitted byte with these source files after exactly those header changes.

To update, select/review a pinned revision, replace the binaries and embedded arrays,
record new sizes/hashes/license and bump the rendering recipe if output changes.
Keep this documentation and tests synchronized. No profile is applied as a pixel
transform; only explicitly sRGB-tagged source samples receive an assigned profile.

Development verification on 2026-10-06: the locally available LittleCMS accepted
both files and evaluated their transfer curves at all 256 8-bit levels. Maximum
absolute difference from the sRGB piecewise transfer function was <0.000003.
LittleCMS is not a build/runtime/test-suite dependency for this change.
