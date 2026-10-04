"""Opt-in MinIO HTTP integration. Own disposable buckets and local server processes."""
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

exe, fixtures = map(lambda p: Path(p).resolve(), sys.argv[1:])
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
        process = subprocess.Popen([str(exe), '--port', str(port), *args], env={**env, **(variables or {})}, stdout=log, stderr=log)
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
    with tempfile.TemporaryDirectory() as directory:
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
            expected_alpha = {fmt:get(local,alpha_path+fmt) for fmt in ('png','jpg')}
            assert all(r[0]==200 for r in expected_alpha.values())
            strip_paths = ['/gray-pyramid.tif/3,1,31,21/15,10/0/default.'+fmt for fmt in ('png','jpg')]
            strip_paths += ['/strip-rgba.tif/3,4,19,8/7,3/0/default.'+fmt for fmt in ('png','jpg')]
            j2k_paths = ['/'+name+'/1,1,35,21/7,4/0/default.'+fmt
                         for name in ('source.jp2','source.j2k','offset.jp2') for fmt in ('png','jpg')]
            expected_j2k = {path:get(local,path) for path in j2k_paths}
            assert all(r[0]==200 for r in expected_j2k.values())
            expected_strips = {path:get(local,path) for path in strip_paths}
            assert all(r[0]==200 for r in expected_strips.values())
        cache = Path(directory, 'cache')
        with server(['--preflight-script', str(policy), '--cache-dir', str(cache)], {
            'SIPI_IIIF_STORAGE': 's3', 'SIPI_S3_BUCKET': bucket, 'SIPI_S3_REGION': 'us-east-1',
            'SIPI_S3_ENDPOINT': endpoint, 'SIPI_S3_PATH_STYLE': 'true',
            'SIPI_S3_ACCESS_KEY': 'sipi-test', 'SIPI_S3_SECRET_KEY': 'sipi-local-test-password',
        }) as remote:
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
            for path, expected_response in expected_j2k.items():
                actual = get(remote,path)
                assert actual[0] == 200 and actual[2] == expected_response[2]
            actual_pyramid = get(remote, pyramid_path)
            assert actual_pyramid[0] == 200 and actual_pyramid[2] == expected_pyramid[2]
            actual_png = get(remote, pyramid_path.replace('.jpg', '.png'))
            assert actual_png[0] == 200 and actual_png[2] == expected_png[2]
            for fmt in ('png','jpg'):
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
    for key in ('tiled.tif', 'museum/selected.tif', 'pyramid.tif', 'alpha-pyramid.tif', 'gray-pyramid.tif', 'strip-rgba.tif', 'source.jp2', 'source.j2k', 'offset.jp2'):
        s3('DELETE', key)
    s3('DELETE')
print('S3 HTTP info, JPEG, HEAD, Lua overrides and prefix mapping passed')
