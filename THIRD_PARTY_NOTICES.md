# Third-party notices

This software is based in part on the work of the Independent JPEG Group.

SIPI-2 incorporates third-party software under its respective licenses. These
licenses are not replaced by SIPI-2's AGPL declaration. Full upstream notices
must accompany binary distributions, including statically linked and header-only
components. No endorsement by upstream authors is implied.

## Conan dependencies

The table combines the macOS and Linux host dependency inventories resolved on
2026-10-14, not a claim that every upstream tool or optional module is shipped.
License names below are recipe metadata; upstream texts determine actual terms.
In particular, XZ's liblzma is 0BSD; the recipe's broad metadata also lists
licenses of upstream command-line/build tools. The runtime does not ship those
XZ tools. See the supplemental COPYING and COPYING.0BSD texts.

| Component | Conan-declared license(s) |
| --- | --- |
| aws-c-auth 0.9.1 | Apache-2.0 |
| aws-c-cal 0.9.8 | Apache-2.0 |
| aws-c-common 0.12.5 | Apache-2.0 |
| aws-c-compression 0.3.1 | Apache-2.0 |
| aws-c-event-stream 0.5.7 | Apache-2.0 |
| aws-c-http 0.10.5 | Apache-2.0 |
| aws-c-io 0.23.2 | Apache-2.0 |
| aws-c-mqtt 0.13.3 | Apache-2.0 |
| aws-c-s3 0.9.2 | Apache-2.0 |
| aws-c-sdkutils 0.2.4 | Apache-2.0 |
| aws-checksums 0.2.6 | Apache-2.0 |
| aws-crt-cpp 0.35.2 | Apache-2.0 |
| aws-sdk-cpp 1.11.692 | Apache-2.0 |
| c-ares 1.34.8 | MIT |
| cli11 2.6.2 | BSD-3-Clause |
| drogon 1.9.13 | MIT |
| fmt 12.1.0 | MIT |
| grok 20.4.15 | AGPL-3.0-only |
| jsoncpp 1.9.5 | MIT |
| jwt-cpp 0.7.2 | MIT |
| lcms 2.19.1 | MIT |
| libcurl 8.22.0 | curl |
| libjpeg-turbo 3.2.0 | IJG, BSD-3-Clause, Zlib |
| libpng 1.6.58 | libpng-2.0 |
| libtiff 4.7.2 | libtiff |
| libwebp 1.6.0 | BSD-3-Clause |
| lua 5.4.8 | MIT |
| openssl 3.6.5 | Apache-2.0 |
| spdlog 1.17.0 | MIT |
| s2n 1.5.27 (Linux) | Apache-2.0 |
| sqlite3 3.53.4 | Unlicense |
| trantor 1.5.28 | BSD-3-Clause |
| util-linux-libuuid 2.39.2 (Linux) | BSD-3-Clause |
| xz_utils 5.8.4 | Unlicense, LGPL-2.1-or-later, GPL-2.0-or-later, GPL-3.0-or-later |
| zlib 1.3.2 | Zlib |

## Embedded components and supplemental notices

- Grok 20.4.15 embeds pinned spdlog, Taskflow and Highway source trees. Their
  license texts are included under `third_party/notices/grok-20.4.15/` in addition
  to Grok's AGPL text. Embedded source-file notices (including OpenJPH BSD
  headers) are retained in `EMBEDDED_NOTICES.txt`. Source URLs and checksums are in its Conan recipe data and
  the supplemental manifest. These are distinct from the top-level spdlog package.
- The sRGB/gray profiles embedded in SIPI-2 come from Compact ICC Profiles by
  Clinton Ingram under CC0-1.0; see [provenance](third_party/compact-icc/README.md)
  and the [license](third_party/compact-icc/LICENSE).
- Supplemental AWS notices and XZ license texts omitted by their Conan package
  license directories are preserved verbatim in [third_party/notices](third_party/notices/README.md).

## Runtime image locations

The Docker build collects the installed host graph's notices into
`/opt/sipi/licenses/`. This includes project terms, ICC notices, every Conan host
package's `licenses/` directory and the version-checked supplemental files.
`inventory.json` records recipe/package revisions, upstream source archive
metadata and notice checksums without private build-machine paths.
The inventory is a conservative dependency superset, not a linker map or legal
compatibility certification. A missing license directory or stale supplement
fails the build instead of silently omitting it.

Ubuntu packages retain their notices under `/usr/share/doc/<package>/copyright`
and `/usr/share/common-licenses/`. The runtime package names, versions and source
package identities are recorded in `/opt/sipi/licenses/ubuntu-packages.tsv`.
The Ubuntu base image is a separate distribution with its own components/terms.

Example after building a local image:

```sh
docker run --rm --entrypoint cat IMAGE /opt/sipi/licenses/inventory.json
```

## Source availability and updates

The notices collection is not a corresponding-source archive.
Use [make source-package](docs/source-package.md) to bundle actual library sources
and recipes for both Linux graphs. Before publishing,
provide access to the exact SIPI-2 source and required dependency sources,
including build recipes and patches, associated with the actual image digest.
Retain the resolved Linux graph for each architecture; a macOS graph alone is
not sufficient release evidence. See [licensing](LICENSING.md) and the
[release readiness review](docs/release-readiness.md).

On dependency updates, refresh both this summary and the reviewed supplemental
notices; do not merely edit reference strings to bypass a mismatch. Upstream
source metadata helps locate originals but does not guarantee future availability.
