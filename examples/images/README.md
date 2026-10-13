# Public quickstart image

`sample.png` is an original procedural 320 × 200 RGB8 test pattern generated for
SIPI-2's documentation. It contains a horizontal red ramp, a vertical green ramp,
and alternating blue squares, with an explicit sRGB declaration. It is not derived
from the private photographic/map benchmark corpus and contains no EXIF metadata.
It is included under the repository's license.

The following Python 3 standard-library code regenerates the image from this
directory. Pixel values are deterministic; compressed bytes can depend on zlib.

```python
from pathlib import Path
import struct
import zlib

width, height = 320, 200
rows = bytearray()
for y in range(height):
    rows.append(0)  # PNG filter type: none.
    for x in range(width):
        rows.extend((x * 255 // (width - 1), y * 255 // (height - 1),
                     80 + 100 * ((x // 40 + y // 40) % 2)))


def chunk(kind: bytes, payload: bytes) -> bytes:
    """Encode a PNG chunk with its length and CRC; no external state is changed."""
    checksum = zlib.crc32(kind + payload) & 0xffffffff
    return struct.pack("!I", len(payload)) + kind + payload + struct.pack("!I", checksum)


header = struct.pack("!2I5B", width, height, 8, 2, 0, 0, 0)
image = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
         + chunk(b"sRGB", b"\0") + chunk(b"IDAT", zlib.compress(rows, 9))
         + chunk(b"IEND", b""))
Path("sample.png").write_bytes(image)
```
