# Supplemental upstream notices

These files are copied verbatim from the pinned upstream source trees in the
Conan cache. They supplement license texts already shipped by Conan packages;
no upstream license terms are modified. `manifest.json` records exact recipe
references, original paths, source archive metadata and SHA-256 checksums.

Grok's three embedded source trees have their own licenses. AWS packages need
upstream NOTICE files in addition to the Apache license text. XZ's COPYING refers
to separate full license texts; they are retained here even where the runtime
uses only the 0BSD liblzma portion.

`scripts/collect_licenses.py` verifies recipe references and file hashes before
including these files in a runtime image. When dependencies change, inspect the
new upstream sources and regenerate reviewed entries. These notices are not
relicensed under SIPI-2's software or documentation license.
