# Configurable upscaling

Upscaling is now rendered for every admitted TIFF/JPEG 2000/JPEG/PNG source,
through the same local/S3 pipeline and JPEG/PNG output writers.

```toml
size-upscaling = true
```

Equivalent settings are --size-upscaling=true and SIPI_SIZE_UPSCALING=true.
Precedence remains CLI > environment > TOML > default; the default is false.
A request must also use the IIIF ^ prefix when its output exceeds the crop.
Without ^, explicit enlargement returns 400; with ^ but disabled policy it returns
501 upscaling_disabled. A ^ request that does not actually enlarge is still valid
when the setting is false. Information advertises sizeUpscaling only when enabled.

For a 300 by 200 image:

| Request size | Result before rotation |
| --- | --- |
| ^600, | 600 by 400 |
| ^,400 | 600 by 400 |
| ^pct:200 | 600 by 400 |
| ^!600,600 | 600 by 400 |
| ^600,100 | 600 by 100, intentionally distorted |
| max | At most 300 by 200, possibly smaller under limits |
| ^max | Largest aspect-preserving size allowed by configured limits |

maxWidth/maxHeight/maxArea and existing working/rotation budgets remain binding.
^max is finite under server policy; it does not mean the unscaled source size.
Explicit oversized outputs fail rather than silently shrinking. As before, adaptive
max/fit forms respect limits, and output sizes describe dimensions before rotation.

## Pixel and memory semantics

resize_image in image/AreaResize.* extends the existing resampling module. It
uses linear interpolation on growing axes, area averaging on shrinking axes;
when both axes grow, interpolation is bilinear. A mixed shrink/grow request uses
both filters directly in one weighted accumulation, without a quantized intermediate
raster. Pure reduction/identity delegates to the old downsample_area unchanged.

Output pixel centers map through the requested window using the half-pixel
convention. Samples clamp at the crop's enclosing pixel centers. Integer crops
therefore never sample neighboring pixels outside the crop, including one-pixel
crops. Fractional windows retain rational area boundaries on shrinking axes.
TIFF/J2K planners select full resolution when either requested axis needs enlargement;
JPEG/PNG retain full decoding and window cropping.

Gray/RGB and straight gray-alpha/RGBA are supported. Interpolation weights colors
by alpha before accumulation, then returns straight color; quantized zero-alpha
output has zero hidden color. Identity keeps all bytes. Arithmetic uses stored
channel values, not linear-light conversion. ICC, explicit sRGB and layout are
preserved; grayscale/bitonal processing still runs after resize and rotation.
The default 256 MiB stage budget includes input + output + ICC and is checked
before allocating the output. Only constant-size filter state is added.

Authorization, geometry and budgets precede cache lookup. Restarting with upscaling
disabled or lower limits cannot reuse a now-forbidden cached result. Canonical
links use explicit ^width,height or ^max as appropriate. No cache recipe bump is
needed: previously successful reductions retain their algorithm/bytes, while
actual enlargement previously never populated the result cache.

## Validation

UpscaleTests checks known bilinear values, mixed-axis area filtering, crop edges,
single-pixel axes, alpha fringes, gray-alpha, metadata and exact memory bounds.
HTTP tests cover size forms, feature advertisement, rotated output, canonical replay,
JPEG/PNG, bitonal, HEAD, cache/restart, denial and lowered limits. The MinIO suite
compares local/S3 results for all four input families, including TIFF pyramids,
alpha/gray and offset JPEG 2000 grids. Existing reduction regressions still pass.
The earlier consortium validator report tested the default disabled policy; its
results must not be presented as external validation of this new capability.
