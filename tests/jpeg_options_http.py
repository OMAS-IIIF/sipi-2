"""JPEG settings across restarts: actual SOF/DQT markers and persistent cache reuse.

Uses the standard-library HTTP fixture harness; no Pillow dependency. Full codec
decoding, odd dimensions and ICC preservation are covered by JpegWriterTests.
"""
from pathlib import Path
import shutil
import sys
import tempfile

from cache_http import server, get, rows, success


def jpeg_headers(data):
    """Read baseline component sampling and quantization tables before scan data."""
    assert data[:2] == b'\xff\xd8'
    offset = 2
    sampling, tables = None, []
    while offset < len(data):
        assert data[offset] == 255
        marker = data[offset + 1]
        length = int.from_bytes(data[offset + 2:offset + 4], 'big')
        payload = data[offset + 4:offset + 2 + length]
        assert length >= 2 and len(payload) == length - 2
        if marker == 0xC0:
            components = payload[5]
            sampling = [payload[7 + 3*i] for i in range(components)]
        elif marker == 0xDB:
            tables.append(payload)
        elif marker == 0xDA:
            break
        offset += 2 + length
    assert sampling and tables
    return sampling, tables


def main():
    executable, fixtures = (Path(p).resolve() for p in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='sipi-jpeg-options-') as temporary:
        root = Path(temporary)
        shutil.copytree(fixtures, root/'images')
        (root/'policy.lua').write_text("function preflight(r) return {decision='allow'} end")
        color = '/tiled.tif/17,29,257,131/max/0/default.jpg'
        gray = '/tiled.tif/full/80,/0/gray.jpg'
        png = '/tiled.tif/full/80,/0/default.png'
        (root/'settings.toml').write_text('jpeg-quality = 70\njpeg-sampling = "4:4:4"\n')
        snapshots = []
        # First default fill, quality change, sampling change, then restore defaults.
        for quality, sampling in ((90, '4:4:4'), (70, '4:4:4'), (70, '4:2:0'), (90, '4:4:4')):
            args, environment = [], {}
            if len(snapshots) in (1, 2):
                args = ['--config', 'settings.toml']
            if len(snapshots) == 2:
                environment = {'SIPI_JPEG_QUALITY': '70', 'SIPI_JPEG_SAMPLING': '4:2:0'}
            if len(snapshots) == 3:
                args = ['--config', 'settings.toml', '--jpeg-quality=90', '--jpeg-sampling=4:4:4']
                environment = {'SIPI_JPEG_QUALITY': 'invalid', 'SIPI_JPEG_SAMPLING': 'invalid'}
            with server(executable, root, args, variables=environment) as (port, log):
                rgb = success(get(port, color), 'image/jpeg')
                gray_body = success(get(port, gray), 'image/jpeg')
                png_body = success(get(port, png))
                assert success(get(port, color), 'image/jpeg') == rgb
                assert jpeg_headers(rgb)[0] == ([0x11, 0x11, 0x11] if sampling == '4:4:4' else [0x22, 0x11, 0x11])
                assert jpeg_headers(gray_body)[0] == [0x11]
                snapshots.append((rgb, gray_body, png_body))
            assert 'IIIF result cache hit' in log[0]
            if len(snapshots) == 4:
                assert 'IIIF result cache stored' not in log[0], 'Restored defaults must reuse original entries'
            assert len(rows(root)) == (3, 5, 6, 6)[len(snapshots)-1]
        assert jpeg_headers(snapshots[0][0])[1] != jpeg_headers(snapshots[1][0])[1], 'Quality must change quantization'
        assert jpeg_headers(snapshots[1][0])[1] == jpeg_headers(snapshots[2][0])[1]
        assert snapshots[1][1] == snapshots[2][1], 'Gray ignores sampling'
        assert all(s[2] == snapshots[0][2] for s in snapshots), 'JPEG settings must not change PNG'
        assert snapshots[3] == snapshots[0], 'Restart must recover the original cache variant'
    print('JPEG quantization/sampling and persistent variant checks passed')


if __name__ == '__main__':
    main()
