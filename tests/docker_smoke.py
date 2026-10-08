"""Opt-in built-image smoke test: read-only runtime, OLDAP policy and HTTP delivery.

Run: python3 tests/docker_smoke.py sipi-2:local-test
Uses only generated fixture data and public test credentials; removes its container.
"""
import base64
import hashlib
import hmac
import http.client
import json
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid
from png_metadata import fixture


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else 'sipi-2:local-test'
    cmake = (Path(__file__).resolve().parents[1]/'CMakeLists.txt').read_text()
    expected_version = re.search(r'project\(sipi-2 VERSION ([0-9.]+)', cmake).group(1)
    version = subprocess.check_output(['docker','run','--rm',image,'--version'], text=True).strip()
    assert version == 'SIPI-2 '+expected_version, version
    name = 'sipi-smoke-' + uuid.uuid4().hex[:12]
    key = 'public-container-test-key-at-least-32-bytes'
    def encode(value):
        return base64.urlsafe_b64encode(json.dumps(value).encode()).rstrip(b'=')
    payload = encode({'alg':'HS256','typ':'JWT'}) + b'.' + encode({
        'typ':'media', 'id':'asset', 'path':'project', 'derivativeName':'master.png',
        'permval':2, 'iss':'https://oldap.org', 'aud':'oldap-api-media',
        'exp':int(time.time())+300})
    signature = base64.urlsafe_b64encode(hmac.new(key.encode(),payload,hashlib.sha256).digest()).rstrip(b'=')
    token = (payload+b'.'+signature).decode()
    with tempfile.TemporaryDirectory(prefix='sipi-docker-') as temporary:
        root = Path(temporary)
        root.chmod(0o755)
        source = root/'project/asset/derived/master.png'
        source.parent.mkdir(parents=True)
        source.write_bytes(fixture())
        # Exercise the new native cache API as well as the packaged OLDAP policy.
        policy = subprocess.check_output(['docker','run','--rm','--entrypoint','cat',
            image,'/opt/sipi/examples/oldap/preflight.lua'],text=True)
        (root/'probe.lua').write_text(policy + '''
local original_preflight = preflight
function preflight(request)
    local cached = sipi.cache.get_or_load('container-probe', function()
        return {value='shared'}, 60
    end)
    assert(cached.value == 'shared')
    local transient = sipi.cache.get_or_load('transient-probe', function()
      return {value='once'}, 0
    end)
    assert(transient.value == 'once' and sipi.cache.get('transient-probe') == nil)
    return original_preflight(request)
end
''')
        env = root/'runtime.env'
        env.write_text('OLDAP_MEDIA_JWT_SECRET='+key+'\nSIPI_PORT=8182\n')
        env.chmod(0o600)
        try:
            subprocess.run(['docker','run','-d','--name',name,'--read-only','--tmpfs','/tmp',
                '--workdir','/tmp','--cap-drop=ALL','--security-opt=no-new-privileges:true',
                '-p','127.0.0.1::8182','--env-file',str(env),
                '--mount',f'type=bind,src={root},dst=/data/images,readonly',
                '--entrypoint','/bin/sh',image,'/opt/sipi/examples/oldap/docker-entrypoint.sh',
                '--iiif-root=/data/images','--cache-dir=/tmp/cache',
                '--preflight-script=/data/images/probe.lua',
                '--public-base-url=http://images.example'],check=True,capture_output=True)
            port = int(subprocess.check_output(['docker','port',name,'8182/tcp'],text=True).strip().rsplit(':',1)[1])
            def request(path):
                conn = http.client.HTTPConnection('127.0.0.1',port,timeout=5)
                try:
                    conn.request('GET',path)
                    response = conn.getresponse()
                    return response.status,dict((k.lower(),v) for k,v in response.getheaders()),response.read()
                finally:
                    conn.close()
            for _ in range(100):
                try:
                    if request('/health')[0] == 200: break
                except OSError:
                    pass
                time.sleep(.1)
            else:
                raise AssertionError('Container health timeout')
            assert json.loads(request('/health')[2])['version'] == expected_version
            base = '/iiif/3/asset'
            status, _, body = request(base+'/info.json?token='+token)
            assert status == 200, (status,body)
            info = json.loads(body)
            assert info['id'] == 'http://images.example'+base and info['width'] == 3
            for _ in range(2):
                status, headers, body = request(base+'/full/max/0/default.png?token='+token)
                assert status == 200 and body.startswith(b'\x89PNG'), (status,body)
                assert token not in headers.get('link','')
                assert request(base+'/full/max/0/default.png')[0] == 403
            status, headers, _ = request(base+'?token='+token)
            assert status == 303 and headers['location'] == info['id']+'/info.json?token='+token
            print('Container smoke passed: health, port 8182, OLDAP JWT, prefix, info, PNG, cache and denial')
        finally:
            subprocess.run(['docker','rm','-f',name],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)


if __name__ == '__main__':
    main()
