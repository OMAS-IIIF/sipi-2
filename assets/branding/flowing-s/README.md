# SIPI-2 flowing S — concept 02

User-requested revision of the tile-S exploration: softer curves, rounded
terminals, and more color. The continuous S is divided into six image tiles with
transparent seams. Turquoise, cyan, blue, violet and magenta follow the shape.
The refined contour uses rotationally matched upper and lower lobes and a 43-unit
stroke (previously 39) for a more balanced, slightly heavier mark. Colors and tile seams remain unchanged. The colorful S now serves as the first
letter of the name, followed by IPI-2; the duplicate lettering S is removed.
This remains a design draft, not an adopted project identity. Concept 01 remains
in `../kachel-s/` for comparison.

## Assets and editing

- `sipi-2-logo-*.svg`: horizontal lockup, 642 × 256 viewBox.
- `sipi-2-icon-*.svg`: standalone S, 256 × 256 viewBox.
- Matching transparent PNGs: 1926 × 768 for logos, 1024 × 1024 for icons.
- `color`: navy wordmark for light backgrounds.
- `dark`: off-white wordmark for dark backgrounds.
- `mono`: all shapes use #142B3B.
- `concept-board.*`: presentation with light/dark, mono and small-size samples.

SVG files are editable masters. A vector mask applies the continuous S contour
to six colored rectangles; no raster images or fonts are embedded. The original
wordmark uses rounded stroked paths. Preserve strokes when editing or expand them
in a vector editor for applications that require filled outlines. SVG masks have
local IDs; prefix them when embedding multiple SVG contents in one HTML document.
Separate image elements do not share ID scopes.

Palette, top to bottom: #27C9B1, #14B8BB, #289DDD, #526CE0, #8057D5, #BD59B9.
Wordmark colors: #142B3B and #F5F7FA. Board labels use system fonts only.

Regenerate with librsvg:

```sh
rsvg-convert -w 1926 -o sipi-2-logo-color.png sipi-2-logo-color.svg
rsvg-convert -w 1024 -o sipi-2-icon-color.png sipi-2-icon-color.svg
rsvg-convert -o concept-board.png concept-board.svg
```

The board includes 64 px and 32 px icon canvases. Below 32 px, a simplified
favicon should be evaluated because thin seams become subpixel details.
No runtime or website integration is included.
