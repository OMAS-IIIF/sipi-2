"""Opt-in local OLDAP measurement using an actual OpenSeadragon browser trace.

Serves a loopback-only viewer; open the printed URL and click Start. After capture,
replays its unique JPEG requests through Caddy with anonymous and locally signed,
image-scoped media credentials. The latter control uses only the permission/path
returned by the real anonymous API lookup. Credentials stay in memory, never in
URLs, output files or diagnostics. No deployment/cache/configuration is changed.
Requires the local OLDAP Docker trial and its installed OpenSeadragon JS bundle.
"""
import argparse
import base64
from concurrent.futures import ThreadPoolExecutor
import hashlib
import hmac
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from pathlib import Path
import statistics
import subprocess
import threading
import time
from urllib.parse import urlsplit, quote


def request(origin, path, headers=None, body=None):
    """Time a fresh connection through complete body receipt; never log headers."""
    parsed = urlsplit(origin)
    cls = http.client.HTTPSConnection if parsed.scheme == 'https' else http.client.HTTPConnection
    connection = cls(parsed.hostname, parsed.port, timeout=15)
    begin = time.perf_counter()
    try:
        connection.request('POST' if body is not None else 'GET', path, body=body, headers=headers or {})
        response = connection.getresponse()
        data = response.read()
        elapsed = (time.perf_counter()-begin)*1000
        if response.status != 200:
            raise RuntimeError(f'HTTP status {response.status}')
        return data, elapsed
    finally:
        connection.close()


def summary(values):
    """Nearest-rank p95 and median; retain raw samples in results.json."""
    ordered = sorted(values)
    return {'count':len(values), 'p50_ms':statistics.median(values),
            'p95_ms':ordered[math.ceil(.95*len(ordered))-1]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--osd', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--origin', default='http://localhost:8088')
    parser.add_argument('--identifier', default='UtCbbUM5Po4g')
    parser.add_argument('--container', default='oldap-imageserver')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    base = '/iiif/3/'+quote(args.identifier,safe='')
    capture, captured = {}, threading.Event()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            if self.path == '/':
                data, mime = Path(__file__).with_suffix('.html').read_bytes(), 'text/html'
            elif self.path == '/openseadragon.min.js':
                data, mime = args.osd.read_bytes(), 'application/javascript'
            elif self.path == '/config':
                data = json.dumps({'info':args.origin+base+'/info.json','base':args.origin+base}).encode()
                mime = 'application/json'
            else:
                self.send_error(404); return
            self.send_response(200); self.send_header('Content-Type',mime)
            self.send_header('Content-Length',str(len(data))); self.end_headers(); self.wfile.write(data)

        def do_POST(self):
            size = int(self.headers.get('Content-Length','0'))
            if self.path != '/capture' or not 0 < size <= 1024*1024 or captured.is_set():
                self.send_error(400); return
            value = json.loads(self.rfile.read(size))
            if value.get('errors') or not value.get('resources'):
                self.send_error(400); return
            capture.update(value)
            self.send_response(200); self.end_headers(); captured.set()

    server = ThreadingHTTPServer(('127.0.0.1',0),Handler)
    threading.Thread(target=server.serve_forever,daemon=True).start()
    print(f'Open http://127.0.0.1:{server.server_port} and click Start measurement',flush=True)
    try:
        if not captured.wait(600): raise RuntimeError('No browser capture within ten minutes')
        paths = sorted({x['path'] for x in capture['resources'] if x['path'].endswith('.jpg')})
        if not paths or any(not p.startswith(base+'/') or '?' in p for p in paths):
            raise RuntimeError('Unexpected captured image paths')
        # Docker output contains secrets. Parse in memory; persist only image ID.
        inspect = json.loads(subprocess.check_output(['docker','inspect',args.container]))[0]
        env = dict(item.split('=',1) for item in inspect['Config']['Env'])
        api = env['OLDAP_API_URL'].rstrip('/').replace('host.docker.internal','127.0.0.1')
        login_body = json.dumps({'password':env.get('OLDAP_UNKNOWN_PASSWORD','')}).encode()
        login, login_ms = request(api,'/admin/auth/'+quote(env.get('OLDAP_UNKNOWN_USERID','unknown'),safe=''),
                                  {'Content-Type':'application/json'},login_body)
        login = json.loads(login)
        access = login.get('accessToken',login.get('token'))
        if not access: raise RuntimeError('Login did not return a credential')
        api_headers = {'Authorization':'Bearer '+access, 'Accept':'application/json'}
        api_path = '/data/mediaobject/id/'+quote(args.identifier,safe='')
        object_data, _ = request(api,api_path,api_headers)
        obj = json.loads(object_data)
        def scalar(value):
            return value[0] if isinstance(value,list) and len(value)==1 else value
        if not isinstance(obj['permval'],int) or obj['permval'] < 1:
            raise RuntimeError('Anonymous image access not granted')
        def encoded(value):
            return base64.urlsafe_b64encode(json.dumps(value,separators=(',',':')).encode()).rstrip(b'=')
        claims = {'typ':'media','id':args.identifier,'path':scalar(obj['shared:path']),
                  'derivativeName':scalar(obj['shared:derivativeName']),'permval':obj['permval'],
                  'iss':env.get('SIPI_PREFLIGHT_CONFIG_JWT_ISSUER',env.get('OLDAP_JWT_ISSUER','https://oldap.org')),
                  'aud':env.get('SIPI_PREFLIGHT_CONFIG_JWT_AUDIENCE',env.get('OLDAP_MEDIA_JWT_AUDIENCE','oldap-api-media')),
                  'exp':int(time.time())+300}
        signing_key = env.get('SIPI_PREFLIGHT_SECRETS_MEDIA_JWT_KEY',env.get('OLDAP_MEDIA_JWT_SECRET'))
        if not signing_key: raise RuntimeError('Expected local trial signing key unavailable')
        payload = encoded({'alg':'HS256','typ':'JWT'})+b'.'+encoded(claims)
        signature = base64.urlsafe_b64encode(hmac.new(signing_key.encode(),payload,hashlib.sha256).digest()).rstrip(b'=')
        media_headers = {'Authorization':'Bearer '+(payload+b'.'+signature).decode()}
        references = {}
        for path in paths:
            data,_ = request(args.origin,path)
            if not data.startswith(b'\xff\xd8'): raise RuntimeError('Expected JPEG')
            references[path] = hashlib.sha256(data).hexdigest()
        results = {'complete':False,'image_id':inspect['Image'],'identifier':args.identifier,
                   'capture':capture,'unique_image_requests':len(paths),'login_ms':login_ms,
                   'warmup':'All captured JPEG URLs fetched before replay; existing server/OS caches retained.',
                   'trials':[]}
        def image_call(path,headers):
            data,elapsed = request(args.origin,path,headers)
            if hashlib.sha256(data).hexdigest()!=references[path]:
                raise RuntimeError('Image bytes differ across authorization paths')
            return elapsed
        # Alternate order to reduce systematic warmup/time bias. Fresh client TCP
        # connections are identical across modes; this is not browser keep-alive.
        for clients in (1,6):
            for repetition in range(5):
                modes = ['anonymous','media-token','api-only']
                if repetition%2: modes.reverse()
                for mode in modes:
                    started=time.perf_counter()
                    with ThreadPoolExecutor(max_workers=clients) as pool:
                        if mode=='api-only':
                            values=list(pool.map(lambda _:request(api,api_path,api_headers)[1],paths))
                        else:
                            headers=media_headers if mode=='media-token' else {}
                            values=list(pool.map(lambda path:image_call(path,headers),paths))
                    trial={'clients':clients,'repetition':repetition,'mode':mode,
                           'wall_ms':(time.perf_counter()-started)*1000,'samples_ms':values,**summary(values)}
                    results['trials'].append(trial)
        results['complete']=True
        (args.output/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        for clients in (1,6):
            for mode in ('anonymous','media-token','api-only'):
                trials=[t for t in results['trials'] if t['clients']==clients and t['mode']==mode]
                values=[v for t in trials for v in t['samples_ms']]
                print(json.dumps({'clients':clients,'mode':mode,**summary(values),
                    'median_batch_ms':statistics.median(t['wall_ms'] for t in trials)}),flush=True)
        print(f'Complete: {len(paths)} unique browser-generated JPEG URLs; results saved.',flush=True)
    finally:
        # Keep the page available until capture/replays complete, then close only
        # this measurement listener. The real server is never restarted.
        server.shutdown(); server.server_close()


if __name__=='__main__':
    main()
