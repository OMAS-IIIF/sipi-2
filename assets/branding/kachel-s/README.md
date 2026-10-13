# SIPI-2 tile S — concept 01

Design exploration, not yet an adopted project identity. Seven rectangular tiles
form an S. The lifted turquoise upper-right tile represents a selected image region.
The wordmark consists of original geometric paths; the logo needs no fonts.

- `sipi-2-logo-*.svg`: horizontal lockups, viewBox 720 × 256.
- `sipi-2-icon-*.svg`: standalone marks, viewBox 256 × 256.
- Matching PNGs: transparent, 2160 × 768 (logo) / 1024 × 1024 (icon).
- `color`: night blue #142B3B and turquoise #20BFA9 for light backgrounds.
- `dark`: off-white #F4F7F8 and turquoise for dark backgrounds.
- `mono`: single night-blue ink; recolor both fills together if needed.
- `concept-board.*`: presentation only, including dark/light and small-size samples.

SVG files are the editable masters. Keep aspect ratio and built-in clear space.
The board uses system fonts for explanatory labels; all logo lettering is paths.
Regenerate PNGs with librsvg, for example:

```sh
rsvg-convert -w 2160 -o sipi-2-logo-color.png sipi-2-logo-color.svg
rsvg-convert -w 1024 -o sipi-2-icon-color.png sipi-2-icon-color.svg
rsvg-convert -o concept-board.png concept-board.svg
```

Visually review the small-size samples before adopting this mark as a favicon;
the tiled structure loses detail below 32 px. No website or application integration
is included in this design draft.
