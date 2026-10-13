"""Opt-in built-image smoke test: read-only runtime, OLDAP policy and HTTP delivery.

Run: python3 tests/docker_smoke.py sipi-2:local-test
Uses only generated fixture data and public test credentials; removes its container.
"""
import base64
import hashlib
import hmac
import http.client
import json
import os
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import uuid
from png_metadata import fixture
from png_check import decode_png
from jpeg_options_http import jpeg_headers


def runtime_contents(image):
    """Inspect the final image, not a builder stage; it must contain only runtime assets."""
    inspected = json.loads(subprocess.check_output(['docker','image','inspect',image],text=True))[0]
    assert inspected['Config']['User'] == 'sipi', inspected['Config']['User']
    platform = os.environ.get('DOCKER_DEFAULT_PLATFORM')
    if platform:
        assert inspected['Os']+'/'+inspected['Architecture'] == platform
    subprocess.run(['docker','run','--rm','--user','0','--entrypoint','/bin/sh',image,'-ec',
        'test ! -e /src; test ! -e /root/.conan2; test ! -e /opt/conan; '
        'for tool in c++ g++ gcc cmake ninja conan; do '
        'if command -v "$tool" >/dev/null; then echo "Unexpected build tool: $tool"; exit 1; fi; done; '
        r'test -z "$(find /opt/sipi -type f \( -name "*.cpp" -o -name "*.h" -o -name "*.a" \))"'],check=True)
    inventory = json.loads(subprocess.check_output(
        ['docker', 'run', '--rm', '--entrypoint', 'cat', image,
         '/opt/sipi/licenses/inventory.json'], text=True))
    assert {'grok', 'libjpeg-turbo', 'aws-sdk-cpp'} <= inventory['components'].keys()
    required = {'LICENSE', 'LICENSING.md', 'THIRD_PARTY_NOTICES.md',
                'compact-icc/LICENSE', 'supplemental-manifest.json'}
    assert required <= inventory['files'].keys()
    checksums = ''.join(f'{digest}  {path}\n' for path, digest in inventory['files'].items())
    subprocess.run(['docker', 'run', '--rm', '-i', '--entrypoint', '/bin/sh', image, '-ec',
                    'cd /opt/sipi/licenses; sha256sum -c - >/dev/null; '
                    'test -s ubuntu-packages.tsv; '
                    'test -s /usr/share/doc/libstdc++6/copyright'],
                   input=checksums, text=True, check=True)
    assert inspected['Config']['Labels']['org.opencontainers.image.licenses'] == 'AGPL-3.0-only'
    print(f'License inventory: {len(inventory["components"])} host packages; checksums verified')
    print(f'Runtime image: {inspected["Architecture"]}, {inspected["Size"]} bytes; no source/build toolchain')


def mounted_policy_smoke(image, root, expected_version):
    """Configure the generic entrypoint using separate read-only media/Lua mounts and env.

    Own and remove one container and cache volume; never touch deployment volumes.
    An arbitrary script selects a public source and denies all other prefixes.
    """
    suffix = uuid.uuid4().hex[:12]
    name, volume = 'sipi-mount-'+suffix, 'sipi-cache-'+suffix
    settings = root/'config'
    settings.mkdir()
    (settings/'preflight.lua').write_text("""function preflight(request)
      if request.prefix[1] ~= 'public' then return {decision='deny'} end
      local source = request.identifier == 'roundtrip' and 'roundtrip.jpg' or 'master.png'
      return {decision='allow',source=source,resolution={max_width=2,max_height=2}}
    end
""")
    subprocess.run(['docker','volume','create',volume],check=True,capture_output=True)
    try:
        subprocess.run(['docker','run','-d','--name',name,'--read-only','--tmpfs','/tmp',
            '--workdir','/tmp','--cap-drop=ALL','--security-opt=no-new-privileges:true',
            '-p','127.0.0.1::8183',
            '--mount',f'type=bind,src={root/"project/asset/derived"},dst=/data/images,readonly',
            '--mount',f'type=bind,src={settings},dst=/etc/sipi,readonly',
            '--mount',f'type=volume,src={volume},dst=/var/cache/sipi',
            '-e','SIPI_PORT=8183','-e','SIPI_IIIF_ROOT=/data/images',
            '-e','SIPI_PREFLIGHT_SCRIPT=/etc/sipi/preflight.lua',
            '-e','SIPI_CACHE_DIR=/var/cache/sipi',
            '-e','SIPI_JPEG_QUALITY=85','-e','SIPI_JPEG_SAMPLING=4:2:0',image],check=True,capture_output=True)
        port=int(subprocess.check_output(['docker','port',name,'8183/tcp'],text=True).strip().rsplit(':',1)[1])
        def request(path):
            connection=http.client.HTTPConnection('127.0.0.1',port,timeout=10)
            try:
                connection.request('GET',path)
                response=connection.getresponse()
                return response.status,response.read()
            finally:
                connection.close()
        for _ in range(150):
            try:
                if request('/health')[0]==200: break
            except OSError:
                pass
            time.sleep(.1)
        else:
            raise AssertionError('Mounted-policy container health timeout')
        assert json.loads(request('/health')[1])['version']==expected_version
        status,body=request('/public/asset/info.json')
        assert status==200 and json.loads(body)['width']==2, (status,body)
        for _ in range(2):
            status,body=request('/public/asset/full/max/0/default.png')
            assert status==200 and body.startswith(b'\x89PNG'), (status,body)
        status,jpeg=request('/public/asset/full/max/0/default.jpg')
        assert status==200 and jpeg_headers(jpeg)[0]==[0x22,0x11,0x11], (status,jpeg)
        # Exercise the packaged decoder too, using our own disposable JPEG output.
        (root/'project/asset/derived/roundtrip.jpg').write_bytes(jpeg)
        status,decoded=request('/public/roundtrip/full/max/0/default.png')
        assert status==200 and decode_png(decoded)[:2]==(2,1), (status,decoded)
        assert request('/original/asset/info.json')[0]==403
        subprocess.run(['docker','exec',name,'test','-s','/var/cache/sipi/cache.sqlite3'],check=True)
        print('Generic runtime mounts passed: media, Lua, environment settings, JPEG 4:2:0 roundtrip, cache and denial')
    finally:
        subprocess.run(['docker','rm','-f',name],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        subprocess.run(['docker','volume','rm',volume],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else 'sipi-2:local-test'
    cmake = (Path(__file__).resolve().parents[1]/'CMakeLists.txt').read_text()
    expected_version = re.search(r'project\(sipi-2 VERSION ([0-9.]+)', cmake).group(1)
    version = subprocess.check_output(['docker','run','--rm',image,'--version'], text=True).strip()
    assert version == 'SIPI-2 '+expected_version, version
    runtime_contents(image)
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
            status,headers,jpeg=request(base+'/full/max/0/default.jpg?token='+token)
            assert status==200 and headers['content-type'].startswith('image/jpeg')
            assert jpeg_headers(jpeg)[0]==[0x11,0x11,0x11]
            status, headers, _ = request(base+'?token='+token)
            assert status == 303 and headers['location'] == info['id']+'/info.json?token='+token
            print('Container smoke passed: health, port 8182, OLDAP JWT, prefix, info, PNG, cache and denial')
        finally:
            subprocess.run(['docker','rm','-f',name],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        mounted_policy_smoke(image,root,expected_version)


if __name__ == '__main__':
    main()
