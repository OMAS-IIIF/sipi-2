# Real-image benchmark preparation

The user-supplied files in `test-images/` are immutable originals. Prepared copies
live under **`build/archive-images/`**, outside the originals and Git's tracked
build inputs. This is offline fixture preparation, not an upload/import feature
or an extension of the server's supported input formats. No performance run is
part of preparation.

The corpus now includes the five initial TIFFs, three historical JP2 files and
a subsequently supplied 229-megapixel Swiss palette GeoTIFF. Keep raw assets out
of release images. The manifest records technical provenance, not a redistribution
license.

### Source attribution and publication review (2026-10-13)

The project owner identified the source groups as follows:

| Original files | Source reported by the owner | Publication status |
| --- | --- | --- |
| `pharch_01`, `pharch_02`, `pharch_03` (TIFF and JP2), `sample-tif-01.tif` | Owner's own images | Record chosen attribution/license before public distribution |
| `sample-tiff-02.tif` | Third-party image; exact source not yet identified | Hold from publication pending source/terms review; retain locally |
| `swiss-map-raster500_500_komb_25_2056.tif` | swisstopo | Hold from publication pending exact dataset/source/terms review; retain locally |

The owner requested keeping both third-party originals locally for tests while
removing them from GitHub and ignoring them in Git. This is not a finding that swisstopo forbids redistribution;
the applicable dataset terms have not been established. The seven owner-supplied
files are not automatically assigned a license by this attribution statement.

All nine originals remain locally available. The two third-party originals were
untracked and given exact root-anchored ignore entries. The owner-approved history
cleanup was pushed on 2026-10-13; both paths and their original blob IDs are absent
from the reachable public `main` history at `d74258c`. The local checkout was
reconciled without changing either original or pending work. The former history
is preserved in a private backup outside the repository. Existing external clones
and GitHub-retained objects are not guaranteed erased by this branch rewrite. Publication exclusion must also cover prepared
derivatives, including benchmark image bundles. Preserve historical measurements and clearly state which optional corpus
is required to reproduce them; never silently replace inputs under old results.
See the [release plan](public-release-plan.md#original-image-provenance).

## Results

All six generated TIFF/JP2 pairs decode to **exactly identical full-resolution
8-bit pixels**. Embedded target ICC bytes also match. All 34 TIFF levels pass an
independent pixel check, including odd-sized boundaries. Nine original files
were hashed before and after preparation and are unchanged.

| Source stem | Full dimensions | Prepared layout | TIFF levels | Pyramid TIFF MiB | Lossless JP2 MiB |
| --- | --- | --- | ---: | ---: | ---: |
| pharch_01 | 1671 × 2212 | RGB8 | 5 | 9.51 | 4.05 |
| pharch_02 | 3684 × 2444 | RGB8 | 5 | 24.98 | 15.21 |
| pharch_03 | 3658 × 3502 | Gray8 | 5 | 13.41 | 7.58 |
| sample-tif-01 | 3684 × 2444 | RGB8 | 5 | 23.90 | 14.38 |
| sample-tiff-02 | 5184 × 3456 | RGB8 | 6 | 49.31 | 22.05 |
| swiss-map-raster500_500_komb_25_2056 | 18080 × 12640 | RGB8 | 8 | 97.83 | 204.44 |

The TIFF includes all pyramid levels; the JP2 includes six wavelet resolutions.
The map illustrates that lossless JPEG2000 is not necessarily smaller than LZW
TIFF. These sizes describe this explicit encoding recipe, not optimum codecs.

[Checked-in preparation manifest](benchmarks/2026-10-08-archive-corpus.json) records
source/output hashes, actual bit depths, profiles, pyramid dimensions, JPEG2000
coding parameters and actual codec versions. Full output and per-file helper logs
are in `build/archive-images/`. The historical JP2 files remain unchanged; their
SIZ/COD/colour headers are inventoried, but they are not the controlled pairs.
Both pharch_01 and pharch_03 historical JP2s are 16-bit and cannot stand in for
SIPI-2's supported 8-bit benchmark inputs. No claim of pixel identity between a
historical JP2 and its original TIFF is made.

## Colour, precision and alpha decisions

- **pharch_01:** original RGB16 plus associated alpha, embedded `Generic RGB
  Profile`. Unassociate RGB at source precision, then LittleCMS relative
  colorimetric conversion with black-point compensation to sRGB8. The alpha range
  is 65528–65535: every alpha value rounds to 255 in 8 bits. Only this redundant
  normalized alpha channel is removed. This corpus therefore does **not** contain
  a genuine transparency case. The helper refuses any alpha that remains
  nonopaque in 8 bits; it never silently chooses a background.
- **pharch_03:** original Gray16 with `EPSON Gray - Gamma 1.8`. Convert at 16-bit
  input precision to Gray8 Gamma 2.2 D50, with the corresponding embedded profile.
  Do not discard low bytes before conversion or assume the source is Gamma 2.2.
- **Untagged RGB8 photographs:** assume sRGB; preserve pixel values exactly and
  attach a common sRGB profile. No automatic contrast adjustment, sharpening,
  denoising or dynamic-range stretching.
- **Swiss map:** expand the first IFD's opaque 8-bit palette to RGB8, then assume
  sRGB. Every original 16-bit palette entry is exactly representable in 8 bits
  (a multiple of 257), so this expansion loses no colour precision. Regenerate
  the pyramid from the expanded full image. Original overview/GeoTIFF tags stay
  in the source; benchmark copies are **not georeferenced GIS replacements**.

Target profiles are ICC v2.1 matrix/gray profiles, with fixed creation timestamps
and cleared optional IDs for reproducible bytes. The helper retains `source.icc`
where present and `target.icc`. Original EXIF/XMP and other archival metadata stay
in the originals; this normalization is not a metadata-preserving archival import.
The 16-to-8-bit conversion is intentionally lossy; only the subsequent TIFF/JP2
encoding of those **same normalized pixels** is lossless.

## Encoding recipe

For each stem:

- `reference.tif`: full-size RGB8/Gray8, uncompressed strips, target ICC.
- `pyramid.tif`: top-left, contiguous, 256×256 tiles, LZW plus horizontal predictor,
  standard main-IFD chain. Each level is a rounded 2×2 average of its parent;
  odd edges average existing samples only, dimensions round up. Stop when both
  axes are ≤256. Every level carries the target ICC.
- `image.jp2`: Grok 20.4.15, lossless reversible 5/3 wavelet, 1024×1024 tiles,
  six resolutions, one layer, LRCP, 64×64 codeblocks, embedded target ICC.
  Explicit reversible component transform for RGB, disabled for gray. Other
  settings are pinned Grok defaults, not a recommended archival compression policy.

TIFF averaged pyramid pixels and reduced JPEG2000 wavelet pixels need not match.
Full-resolution equality does not imply identical reduced-level reconstruction.
The helper is bounded to 300 million input pixels. Encoding can use several GiB
of RAM for the map; these offline allocations do not describe server request RSS.

## Reproduce

Use the existing Release Conan/CMake configuration. No new application dependency
or system package is required. Pillow in the validator venv supplies an independent
OpenJPEG decoder and TIFF verification; actual versions are in the manifest.

```sh
cmake --build build/release --target prepare-archive-image -j 4
build/iiif-validator-venv/bin/python benchmarks/test_prepare_archives.py \
  build/release/prepare-archive-image
build/iiif-validator-venv/bin/python benchmarks/prepare_archives.py \
  --source test-images \
  --output build/archive-images \
  --helper build/release/prepare-archive-image
build/iiif-validator-venv/bin/python benchmarks/archive_http_smoke.py \
  --images build/archive-images \
  --server build/release/sipi-2 \
  --output build/archive-http-2048
```

Choose **new output directories** when repeating; existing directories are refused.
A failed run retains diagnostics and cannot be mistaken for a completed manifest.
The three small offline regressions do not need private source images and cover
odd edges/exact RGB preservation, uint16 full-range conversion, real transparency
rejection and refusal to overwrite. They are opt-in benchmark-tool checks.
`PrepareArchiveImage.cpp` owns TIFF/LCMS/Grok handles and performs normalization
and encoding; `prepare_archives.py` handles provenance and independent validation.

The profile must be checked from JP2's `colr` box: Pillow does not expose the ICC
for these JP2 files and displays 16-bit RGB originals as an 8-bit mode. The audit
therefore reads SIZ/COD/colour boxes, rather than inferring source precision or
missing profiles from Pillow's display mode.

## HTTP validation and a limit to retain for the next benchmark

[HTTP results](benchmarks/2026-10-08-archive-http.json): **48/48** requests pass,
covering both files for all six images: `info.json`, two exact native PNG crops
(one near the bottom/right edge), and a full image reduced to width 256. Crops
match the normalized reference byte-for-byte. Reduced dimensions are checked;
TIFF/JP2 reduced pixel equality is deliberately not required. Only disposable
loopback servers were used; no running OLDAP service/configuration changed.

The initial default-policy run rejected `sample-tiff-02/image.jp2` with HTTP 501,
`source_not_deliverable`: the admission check requires a feasible `full/max`
rendering within its 256 MiB working budget. The default maximum output selects
a JPEG2000 level whose conservative decoding estimate exceeds that budget.
This is **not** invalid JPEG2000 data; independent full decoding already passes.
Initial evidence stays in `build/archive-http-checks/`.

The successful smoke run uses existing configuration:

```text
--max-width 2048 --max-height 2048 --max-area 4194304
```

These are output limits, not source resizing or a reduced public source canvas.
Full-resolution 256px crops remain possible anywhere on the 229 MP source.
The server advertises the limits in its information document. Future comparative
runs must record this admission/output policy and use requests valid for every
server. This finding may warrant a later discussion of admission policy; no
budget or production implementation was changed during fixture preparation.

Next: adapt the comparative runner to variable dimensions, nonaligned crops and
these real pairs. Configure an actual JPEG2000 processor for Cantaloupe before
claiming a four-server JP2 comparison. Retain feature freeze.
