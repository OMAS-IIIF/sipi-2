"""Opt-in MinIO HTTP integration. Own disposable buckets and local server processes."""
from concurrent.futures import ThreadPoolExecutor
import contextlib
import http.client
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time
import uuid
from watermark_preflight_http import mask
from png_check import decode_png

exe, fixtures, jpeg_fixtures, png_fixtures = map(lambda p: Path(p).resolve(), sys.argv[1:])
bucket = 'sipi-http-' + uuid.uuid4().hex
endpoint = 'http://127.0.0.1:9000'
auth = ['curl', '--fail-with-body', '--silent', '--show-error', '--max-time', '10',
        '--aws-sigv4', 'aws:amz:us-east-1:s3', '--user', 'sipi-test:sipi-local-test-password']
backend = ['--s3-bucket', bucket, '--s3-endpoint', endpoint, '--s3-path-style=true',
           '--s3-access-key=sipi-test', '--s3-secret-key=sipi-local-test-password']
env = {k: v for k, v in os.environ.items() if not k.startswith('SIPI_')}


def s3(method, key='', file=None):
    args = auth + ['-X', method, f'{endpoint}/{bucket}' + ('/' + key if key else '')]
    if file:
        args += ['--upload-file', str(file)]
    subprocess.run(args, check=True, capture_output=True)


def get(port, path, method='GET'):
    with contextlib.closing(http.client.HTTPConnection('127.0.0.1', port, timeout=15)) as connection:
        connection.request(method, path)
        response = connection.getresponse()
        return response.status, {k.lower(): v for k, v in response.getheaders()}, response.read()


@contextlib.contextmanager
def server(args, variables=None):
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        port = sock.getsockname()[1]
    with tempfile.TemporaryFile() as log:
        process = subprocess.Popen([str(exe), '--port', str(port), '--size-upscaling=true', *args], env={**env, **(variables or {})}, stdout=log, stderr=log)
        try:
            for _ in range(100):
                if process.poll() is not None:
                    raise AssertionError('Server exited before listening')
                try:
                    if get(port, '/health')[0] == 200:
                        break
                except OSError:
                    pass
                time.sleep(.05)
            else:
                raise AssertionError('Server did not become ready')
            yield port
        finally:
            process.terminate()
            try:
                process.wait(timeout=15)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()


s3('PUT')
try:
    for key in ('tiled.tif', 'museum/selected.tif', 'pyramid.tif', 'alpha-pyramid.tif', 'gray-pyramid.tif', 'strip-rgba.tif', 'source.jp2', 'source.j2k', 'offset.jp2'):
        s3('PUT', key, fixtures / (key if key in ('pyramid.tif','alpha-pyramid.tif', 'gray-pyramid.tif', 'strip-rgba.tif', 'source.jp2', 'source.j2k', 'offset.jp2') else 'tiled.tif'))
    for name in ('rgb.jpg', 'icc.jpg', 'orientation.jpg', 'broken.jpg'):
        s3('PUT', name, jpeg_fixtures / name)
    for name in ('rgb.png', 'icc.png', 'rgba.png', 'adam7-large.png', 'srgb.png', 'srgb-gray.png', 'sixteen.png', 'bad-crc.png'):
        s3('PUT', name, png_fixtures / name)
    with tempfile.TemporaryDirectory() as directory:
        mask_file=Path(directory,'mask.png')
        mask_file.write_bytes(mask(255))
        s3('PUT','watermark.png',mask_file)
        watermark_policy=Path(directory,'watermark.lua')
        watermark_policy.write_text("""function preflight(r)
          local result={decision='allow',source={type='s3',key='rgb.png'}}
          if r.identifier~='plain' then
            result.watermark={source={type='s3',key='watermark.png'},
              width_fraction=0.5,opacity=1,color={255,255,255}}
          end
          return result
        end""")
        with server(backend+['--preflight-script',str(watermark_policy),
                             '--cache-dir',str(Path(directory,'watermark-cache'))]) as marked:
            url='/marked/full/max/0/default.png'
            plain=get(marked,'/plain/full/max/0/default.png')
            first=get(marked,url)
            assert first[0]==200 and plain[0]==200 and first[2]!=plain[2]
            w,h,pixels,*_=decode_png(first[2])
            center=((h//2)*w+w//2)*3
            assert pixels[center:center+3]==bytes([255]*3)
            assert get(marked,url)[2]==first[2]
            mask_file.write_bytes(mask(0)); s3('PUT','watermark.png',mask_file)
            assert get(marked,url)[2]==plain[2]
            s3('DELETE','watermark.png')
            assert get(marked,url)[0]==500
            assert get(marked,'/marked/info.json')[0]==500
        restricted=Path(directory,'restricted.lua')
        restricted.write_text("""function preflight(r)
          if r.prefix[1]~='public' then return {decision='deny'} end
          return {decision='allow',resolution={max_width=20,max_height=20}}
        end""")
        paths=['/'+name+'/full/max/0/default.png' for name in ('tiled.tif','pyramid.tif','source.jp2','offset.jp2')]
        args=['--preflight-script',str(restricted)]
        with server(['--iiif-root',str(fixtures),*args]) as local:
            references=[get(local,'/public'+path) for path in paths]
            assert all(r[0]==200 for r in references)
        with server([*backend,'--iiif-storage=s3',*args,
                     '--cache-dir',str(Path(directory,'restricted-cache'))]) as remote:
            for path,reference in zip(paths,references):
                for _ in range(2):
                    actual=get(remote,'/public'+path)
                    assert actual[0]==200 and actual[2]==reference[2]
                w,h,pixels,*_=decode_png(reference[2])
                assert w<=20 and h<=20
                part=get(remote,'/public'+path.replace('/full/max/','/1,1,3,2/max/'))
                assert part[0]==200
                assert decode_png(part[2])[2]==b''.join(pixels[(y*w+1)*3:(y*w+4)*3] for y in (1,2))
            assert get(remote,paths[0])[0]==403
            # A warm reduced source/result must not survive a changed S3 validator.
            s3('PUT','tiled.tif',fixtures/'gray.tif')
            changed=get(remote,'/public'+paths[0]); assert changed[0]==200 and changed[2]!=references[0][2]
            s3('DELETE','tiled.tif')
            assert get(remote,'/public'+paths[0])[0]==404
            s3('PUT','tiled.tif',fixtures/'tiled.tif')
        policy = Path(directory, 'policy.lua')
        # Lua performs the project-specific prefix mapping explicitly.
        policy.write_text('''function preflight(request)
          if request.identifier == 'deny' then return {decision='deny'} end
          if request.identifier == 'alias' then
            return {decision='allow', source={type='s3', key=request.prefix[1] .. '/selected.tif'}}
          end
          if request.identifier == 'local' then
            return {decision='allow', source=''' + json.dumps(str(fixtures / 'tiled.tif')) + '''}
          end
          return {decision='allow'}
        end''')
        crop = '/50,30,100,80/37,29/0/default.jpg'
        with server(['--iiif-root', str(fixtures)]) as local:
            expected_gray = get(local, '/gray.tif/full/max/0/default.png')
            assert expected_gray[0] == 200
            expected = get(local, '/tiled.tif' + crop)
            assert expected[0] == 200
            pyramid_path = '/pyramid.tif/3,5,65,49/15,12/0/default.jpg'
            expected_pyramid = get(local, pyramid_path)
            assert expected_pyramid[0] == 200
            expected_png = get(local, pyramid_path.replace('.jpg', '.png'))
            assert expected_png[0] == 200
            alpha_path = '/alpha-pyramid.tif/3,1,29,17/14,8/0/default.'
            expected_rotated_alpha = get(local,alpha_path.replace('/0/','/!270/')+'png')
            assert expected_rotated_alpha[0] == 200
            expected_alpha = {fmt:get(local,alpha_path+fmt) for fmt in ('png','jpg','webp')}
            assert all(r[0]==200 for r in expected_alpha.values())
            strip_paths = ['/gray-pyramid.tif/3,1,31,21/15,10/0/default.'+fmt for fmt in ('png','jpg')]
            # Header-only ICC fixture has no transform tags: never silently drop it for WebP.
            assert get(local,'/gray-pyramid.tif/full/max/0/default.webp')[0]==500
            strip_paths += ['/strip-rgba.tif/3,4,19,8/7,3/0/default.'+fmt for fmt in ('png','jpg','webp')]
            j2k_paths = ['/'+name+'/1,1,35,21/7,4/0/default.'+fmt
                         for name in ('source.jp2','source.j2k','offset.jp2') for fmt in ('png','jpg','webp')]
            j2k_paths += [path.replace('/0/', '/!90/') for path in j2k_paths]
            j2k_paths += [path.replace('/0/', '/!37.5/') for path in j2k_paths if '/0/' in path]
            j2k_paths += ['/'+name+'/1,1,10,10/^20,15/0/default.png'
                          for name in ('source.jp2','source.j2k','offset.jp2')]
            strip_paths += ['/'+name+'/1,1,10,10/^20,5/0/default.png'
                            for name in ('tiled.tif','pyramid.tif','alpha-pyramid.tif','gray-pyramid.tif','strip-rgba.tif')]
            expected_j2k = {path:get(local,path) for path in j2k_paths}
            assert all(r[0]==200 for r in expected_j2k.values())
            expected_strips = {path:get(local,path) for path in strip_paths}
            assert all(r[0]==200 for r in expected_strips.values())
        jpeg_paths = ['/'+name+'/3,4,20,12/10,6/!90/default.'+fmt
                      for name in ('rgb.jpg','icc.jpg') for fmt in ('jpg','png','webp')]
        jpeg_paths += ['/rgb.jpg/3,4,10,10/^20,15/0/default.png']
        with server(['--iiif-root', str(jpeg_fixtures)]) as local:
            jpeg_reference = {path:get(local,path) for path in jpeg_paths}
            assert all(result[0] == 200 for result in jpeg_reference.values())
        png_paths = ['/'+name+'/full/2,1/!37.5/default.'+fmt
                     for name in ('rgb.png','icc.png','rgba.png','adam7-large.png','srgb.png','srgb-gray.png') for fmt in ('png','jpg','webp')]
        png_paths += [path.replace('default.',quality+'.') for path in png_paths if 'icc.png' not in path for quality in ('gray','bitonal')]
        png_paths += ['/'+name+'/full/^10,10/0/default.png' for name in ('rgb.png','rgba.png','srgb-gray.png')]
        with server(['--iiif-root', str(png_fixtures)]) as local:
            png_reference = {path:get(local,path) for path in png_paths}
            assert all(result[0] == 200 for result in png_reference.values()), [(path,r[0],r[2]) for path,r in png_reference.items() if r[0]!=200]
        cache = Path(directory, 'cache')
        with server(['--iiif-workers=4', '--preflight-script', str(policy), '--cache-dir', str(cache)], {
            'SIPI_IIIF_STORAGE': 's3', 'SIPI_S3_BUCKET': bucket, 'SIPI_S3_REGION': 'us-east-1',
            'SIPI_S3_ENDPOINT': endpoint, 'SIPI_S3_PATH_STYLE': 'true',
            'SIPI_S3_ACCESS_KEY': 'sipi-test', 'SIPI_S3_SECRET_KEY': 'sipi-local-test-password',
        }) as remote:
            for name in ('rgb.png', 'icc.png'):
                status, _, body = get(remote, '/museum/' + name + '/info.json')
                info = json.loads(body)
                assert status == 200 and (info['width'], info['height']) == (3,2)
                assert info['profile']=='level2'
                assert get(remote, '/' + name + '/info.json', 'HEAD')[0] == 200
                assert get(remote, '/' + name + '/full/max/0/default.png')[0] == 200
            assert get(remote,'/gray-pyramid.tif/full/max/0/default.webp')[0]==500
            assert get(remote, '/sixteen.png/info.json')[0] == 501
            assert get(remote, '/bad-crc.png/info.json')[0] == 500
            for path, reference in {**jpeg_reference, **png_reference}.items():
                actual = get(remote, path)
                assert actual[0] == 200 and actual[2] == reference[2]
            for name in ('rgb.jpg', 'icc.jpg'):
                status, _, body = get(remote, '/' + name + '/info.json')
                assert status == 200
                info = json.loads(body)
                assert (info['width'], info['height']) == (37, 23) and info['profile']=='level2'
                assert get(remote, '/' + name + '/info.json', 'HEAD')[0] == 200
                assert get(remote, '/' + name + '/full/max/0/default.jpg')[0] == 200
            assert get(remote, '/orientation.jpg/info.json')[0] == 501
            assert get(remote, '/broken.jpg/info.json')[0] == 500
            for path in ('/tiled.tif', '/museum/tiled.tif', '/museum/alias', '/local'):
                result = get(remote, path + crop)
                assert result[0] == 200 and result[2] == expected[2], (path, result[0])
            for name in ('source.jp2', 'source.j2k', 'offset.jp2'):
                status, _, body = get(remote, '/museum/'+name+'/info.json')
                info = json.loads(body)
                assert status == 200 and (info['width'], info['height']) == (37, 23)
                assert info['id'].endswith('/museum/'+name)
                assert get(remote, '/'+name+'/info.json', 'HEAD')[0] == 200
                assert get(remote, '/'+name+'/full/max/0/default.jpg')[0] == 200
                assert get(remote, '/'+name+'/100,0,1,1/max/0/default.jpg')[0] == 400
            # Cold concurrent S3 range reads exercise the shared AWS client and
            # independent Grok streams before the subsequent cache-hit checks.
            with ThreadPoolExecutor(max_workers=8) as clients:
                actuals = list(clients.map(lambda path: get(remote, path), list(expected_j2k) * 3))
            for path, actual in zip(list(expected_j2k) * 3, actuals):
                assert actual[0] == 200 and actual[2] == expected_j2k[path][2]
            for path, expected_response in expected_j2k.items():
                actual = get(remote,path)
                assert actual[0] == 200 and actual[2] == expected_response[2]
            actual_pyramid = get(remote, pyramid_path)
            assert actual_pyramid[0] == 200 and actual_pyramid[2] == expected_pyramid[2]
            actual_png = get(remote, pyramid_path.replace('.jpg', '.png'))
            assert actual_png[0] == 200 and actual_png[2] == expected_png[2]
            for _ in range(2):
                actual = get(remote,alpha_path.replace('/0/','/!270/')+'png')
                assert actual[0] == 200 and actual[2] == expected_rotated_alpha[2]
            for fmt in ('png','jpg','webp'):
                actual = get(remote,alpha_path+fmt)
                assert actual[0] == 200 and actual[2] == expected_alpha[fmt][2]
            for path in strip_paths:
                actual=get(remote,path)
                assert actual[0]==200 and actual[2]==expected_strips[path][2]
            status, _, data = get(remote, '/museum/tiled.tif/info.json')
            info = json.loads(data)
            assert status == 200 and info['width'] == 320 and info['height'] == 200
            assert info['id'].endswith('/museum/tiled.tif')
            assert get(remote, '/absent/info.json')[0] == 404
            assert get(remote, '/deny/info.json')[0] == 403
            status, headers, body = get(remote, '/tiled.tif' + crop, 'HEAD')
            assert status == 200 and not body and int(headers['content-length']) == len(expected[2])
            # Repeat the cached S3 response; prefix aliases resolve to the same source.
            assert get(remote, '/tiled.tif' + crop)[2] == expected[2]
            full_path = '/tiled.tif/full/max/0/default.png'
            old_full = get(remote, full_path)
            assert old_full[0] == 200
            with sqlite3.connect(cache / 'cache.sqlite3') as db:
                before = db.execute('SELECT count(*) FROM entries').fetchone()[0]
            assert get(remote, full_path)[2] == old_full[2]
            # A new object ETag/version must prevent a hit under the same IIIF URL.
            s3('PUT', 'tiled.tif', fixtures / 'gray.tif')
            changed = get(remote, full_path)
            assert changed[0] == 200 and changed[2] == expected_gray[2] != old_full[2]
            with sqlite3.connect(cache / 'cache.sqlite3') as db:
                assert db.execute('SELECT count(*) FROM entries').fetchone()[0] == before + 1
            s3('DELETE', 'tiled.tif')
            assert get(remote, full_path)[0] == 404
        with server(backend + ['--iiif-root', str(fixtures), '--preflight-script', str(policy)]) as mixed:
            assert get(mixed, '/museum/alias' + crop)[2] == expected[2]
        # Denial succeeds even when the configured object store is unreachable.
        offline = backend.copy()
        offline[offline.index(endpoint)] = 'http://127.0.0.1:1'
        with server(offline + ['--iiif-storage=s3', '--preflight-script', str(policy)]) as denied:
            assert get(denied, '/deny/info.json')[0] == 403
finally:
    s3('DELETE','watermark.png')
    for key in ('tiled.tif', 'museum/selected.tif', 'pyramid.tif', 'alpha-pyramid.tif', 'gray-pyramid.tif', 'strip-rgba.tif', 'source.jp2', 'source.j2k', 'offset.jp2'):
        s3('DELETE', key)
    for name in ('rgb.jpg', 'icc.jpg', 'orientation.jpg', 'broken.jpg'):
        s3('DELETE', name)
    for name in ('rgb.png', 'icc.png', 'rgba.png', 'adam7-large.png', 'srgb.png', 'srgb-gray.png', 'sixteen.png', 'bad-crc.png'):
        s3('DELETE', name)
    s3('DELETE')
print('S3 HTTP info, JPEG, HEAD, Lua overrides and prefix mapping passed')
