"""JPEG metadata and pixel delivery through the shared processing/cache pipeline."""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import shutil
import sys
import tempfile
from cache_http import server, get, rows
from png_check import decode_png
from rotation_http import transformed_png, jpeg_size


def main():
    executable, fixtures, tiffs = (Path(value).resolve() for value in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='sipi-jpeg-header-') as temporary:
        root = Path(temporary)
        images = root / 'images'
        shutil.copytree(fixtures, images)
        shutil.copyfile(fixtures / 'rgb.jpg', images / 'opaque-id')
        (root / 'policy.lua').write_text("""
function preflight(request)
  if request.headers.authorization == 'denied' then return {decision='deny'} end
  if request.identifier == 'alias' then return {decision='allow', source='gray.jpg'} end
  return {decision='allow'}
end
""")
        with server(executable, root, ['--iiif-workers=4']) as (port, log):
            names = ['rgb.jpg', 'gray.jpg', 'progressive.jpg', 'icc.jpg', 'orientation-one.jpg',
                     'header-only.jpg', 'opaque-id', 'alias']
            def verify(name):
                path = f'/museum/project/{name}/info.json'
                status, headers, body = get(port, path)
                assert status == 200, (name, body)
                info = json.loads(body)
                assert (info['width'], info['height']) == (37, 23)
                assert info['id'].endswith('/museum/project/' + name)
                assert info['profile']=='level2'
                assert info['profile']=='level2'
                assert {k.lower(): v for k, v in headers.items()}['cache-control'] == 'no-store'
                get_length = {k.lower(): v for k, v in headers.items()}['content-length']
                status, head, body = get(port, path, 'HEAD')
                assert status == 200 and not body
                assert {k.lower(): v for k, v in head.items()}['content-length'] == get_length
                status, _, body = get(port, f'/{name}/full/max/0/default.png')
                if name == 'header-only.jpg':
                    assert status == 500
                else:
                    assert status == 200, (name, body)
                    assert decode_png(body)[:2] == (37, 23)
            with ThreadPoolExecutor(max_workers=8) as clients:
                list(clients.map(verify, names * 3))
            for name, expected in [('cmyk.jpg', 501), ('orientation.jpg', 501), ('broken.jpg', 500)]:
                status, _, _ = get(port, f'/{name}/info.json')
                assert status == expected
            assert get(port, '/rgb.jpg/info.json', authorization='denied')[0] == 403
            # Geometry remains format-neutral.
            assert get(port, '/rgb.jpg/100,0,5,5/max/0/default.jpg')[0] == 400
            assert rows(root)
            before = len(rows(root))
            assert get(port, '/header-only.jpg/0,0,1,1/max/0/default.png')[0] == 500
            assert len(rows(root)) == before
            for name in ('rgb.jpg', 'gray.jpg', 'progressive.jpg', 'icc.jpg'):
                full = get(port, f'/{name}/full/max/0/default.png')[2]
                w, h, pixels, profile, _ = decode_png(full)
                channels = len(pixels) // (w*h)
                prefix = f'/{name}/3,4,20,12/'
                cropped = get(port, prefix + 'max/0/default.png')[2]
                expected = b''.join(pixels[((y*w+3)*channels):((y*w+23)*channels)] for y in range(4,16))
                assert decode_png(cropped)[:4] == (20,12,expected,profile)
                reduced = get(port, prefix + '10,6/0/default.png')[2]
                assert decode_png(reduced)[:2] == (10,6)
                path = prefix + '10,6/!90/default.png'
                rotated = get(port, path)[2]
                assert decode_png(rotated)[:4] == transformed_png(reduced, True, 1)
                assert get(port, path)[2] == rotated
                assert get(port, path, 'HEAD')[0] == 200
                jpeg = get(port, prefix + '10,6/!90/default.jpg')
                assert jpeg[0] == 200 and jpeg_size(jpeg[2]) == (6,10)
                if name == 'icc.jpg':
                    assert profile == (fixtures / 'expected.icc').read_bytes()
            # Same ID changes codec/version: cached JPEG facts must not survive replacement.
            shutil.copyfile(tiffs / 'tiled.tif', images / 'replacement')
            (images / 'replacement').replace(images / 'opaque-id')
            info = json.loads(get(port, '/opaque-id/info.json')[2])
            assert (info['width'], info['height']) == (320, 200)
            assert info['profile']=='level2'
            (images / 'rgb.jpg').unlink()
            assert get(port, '/rgb.jpg/info.json')[0] == 404
        assert 'IIIF metadata cache hit' in log[0]
        assert 'IIIF result cache stored' in log[0]
    print('JPEG info/HEAD, policy, capability boundary, cache and replacement checks passed')


if __name__ == '__main__':
    main()
