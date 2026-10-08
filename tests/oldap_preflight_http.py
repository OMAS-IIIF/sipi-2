"""Run the shipped OLDAP policy against real sipi-2 and a deterministic local API."""
from concurrent.futures import ThreadPoolExecutor
import base64
import hashlib
import hmac
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
import subprocess
from pathlib import Path
import sys
import tempfile
import threading
import time
from cache_http import server
from png_metadata import fixture

KEY = 'test-only-media-key-with-at-least-32-bytes'


def token(**changes):
    claims = dict(typ='media', iss='https://oldap.org', aud='oldap-api-media',
                  exp=int(time.time())+300, id='asset', permval=2, path=['project'],
                  derivativeName=['master.png'])
    claims.update(changes)
    def encode(data):
        return base64.urlsafe_b64encode(json.dumps(data).encode()).rstrip(b'=').decode()
    body = encode({'alg':'HS256', 'typ':'JWT'})+'.'+encode(claims)
    signature = base64.urlsafe_b64encode(hmac.new(KEY.encode(), body.encode(), hashlib.sha256).digest()).rstrip(b'=')
    return body+'.'+signature.decode()


class Api(BaseHTTPRequestHandler):
    calls = []
    mode = 'allow'
    user = 'unknown'
    password = ''
    token_field = 'accessToken'
    token = ''
    lifetime = 300
    login_delay = .05
    def log_message(self, *_):
        pass
    def reply(self, status, data):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def do_POST(self):
        Api.calls.append(('POST', self.path))
        assert self.path == '/admin/auth/'+Api.user
        assert self.headers.get('Authorization') is None
        assert json.loads(self.rfile.read(int(self.headers['Content-Length']))) == {'password':Api.password}
        time.sleep(Api.login_delay)
        Api.token = 'anonymous-api-token-'+str(sum(method == 'POST' for method,_ in Api.calls))
        self.reply(200, {Api.token_field:Api.token, 'tokenType':'Bearer', 'expiresIn':Api.lifetime})
    def do_GET(self):
        Api.calls.append(('GET', self.path))
        assert self.path in ('/data/mediaobject/id/asset', '/data/mediaobject/id/other')
        if self.headers.get('Authorization') != 'Bearer '+Api.token or Api.mode == 'unauthorized':
            self.reply(401, {})
            return
        if Api.mode == 'denied':
            self.reply(403, {})
        elif Api.mode == 'broken':
            self.reply(503, {})
        elif Api.mode == 'unsafe':
            self.reply(200, {'permval':2, 'shared:path':'../project', 'shared:derivativeName':'master.png'})
        elif Api.mode == 'restricted':
            self.reply(200, {'permval':0, 'shared:path':'project', 'shared:derivativeName':'master.png'})
        else:
            self.reply(200, {'permval':2, 'shared:path':['project'], 'shared:derivativeName':['master.png']})


def main():
    executable = Path(sys.argv[1]).resolve()
    policy = Path(__file__).resolve().parents[1]/'examples/oldap/preflight.lua'
    api = ThreadingHTTPServer(('127.0.0.1', 0), Api)
    thread = threading.Thread(target=api.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix='sipi-oldap-') as temporary:
            root = Path(temporary)
            image = root/'images/project/asset/derived/master.png'
            image.parent.mkdir(parents=True)
            image.write_bytes(fixture())
            (root/'policy.lua').write_text(policy.read_text())
            env = {'SIPI_PREFLIGHT_SECRETS_MEDIA_JWT_KEY':KEY}
            options = ['--config', str(policy.with_name('sipi.toml')),
                       '--preflight-config=minimum_permission=1',
                       '--preflight-config=anonymous_permission_ttl=0',
                       '--preflight-config=media_cookie=media_capability',
                       f'--preflight-config=api_url=http://127.0.0.1:{api.server_port}']
            with server(executable, root, options, variables=env) as (port, logs):
                def get(query='', headers=None, path='/asset/full/max/0/default.png', method='GET'):
                    conn = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
                    try:
                        conn.request(method, path+query, headers=headers or {})
                        response = conn.getresponse()
                        return response.status, response.read()
                    finally:
                        conn.close()
                good = token()
                for path in ('/asset/full/max/0/default.png', '/project/asset/info.json', '/asset'):
                    for method in ('GET', 'HEAD'):
                        assert get('?token='+good, path=path, method=method)[0] == (303 if path=='/asset' else 200)
                assert not Api.calls
                assert get(headers={'Authorization':'Bearer '+good})[0] == 200
                assert get(headers={'Cookie':'media_capability='+good})[0] == 200
                assert get('?token='+good, {'Authorization':'invalid'})[0] == 200  # Query wins.
                assert get('?token='+token(permval=1))[0] == 200
                for bad in ('', 'wrong', token(id='other'), token(id=None), token(assetId='other'),
                            token(typ='access'), token(iss='other'), token(aud='other'), token(exp=0),
                            token(permval=0), token(permval='2'), token(permval=7),
                            token(path='../project'), token(path='/project'), token(path='project//part'),
                            token(path=['project','other']), token(derivativeName='../master.png'),
                            token(derivativeName=None)):
                    assert get('?token='+bad)[0] == 403
                assert get('?token='+good+'&token='+good)[0] == 403
                assert get('?token=wrong', {'Authorization':'Bearer '+good})[0] == 403
                assert get(headers={'Authorization':'invalid','Cookie':'media_capability='+good})[0] == 403
                assert not Api.calls, 'invalid credentials must not fall back to anonymous'
                with ThreadPoolExecutor(max_workers=12) as pool:
                    results = list(pool.map(lambda _: get()[0], range(12)))
                assert results == [200]*12
                assert sum(method == 'POST' for method,_ in Api.calls) == 1, 'single login for a concurrent miss'
                assert sum(method == 'GET' for method,_ in Api.calls) == 12, 'every image still checks permissions'
                for _ in range(2):
                    assert get()[0] == 200
                assert sum(method == 'POST' for method,_ in Api.calls) == 1, 'warm cache reuses login'
                Api.token = 'revoked'  # Force one 401; refresh then retry the permission lookup.
                assert get()[0] == 200
                assert sum(method == 'POST' for method,_ in Api.calls) == 2
                Api.mode = 'unauthorized'
                before = sum(method == 'POST' for method,_ in Api.calls)
                assert get()[0] == 403
                assert sum(method == 'POST' for method,_ in Api.calls) == before+1, 'retry is bounded'

                Api.mode = 'denied'
                assert get()[0] == 403
                Api.mode = 'broken'
                assert get()[0] == 500
                for mode in ('unsafe', 'restricted'):
                    Api.mode = mode
                    assert get()[0] == 403
                Api.mode = 'allow'
                Api.token_field = 'token'
                Api.token = 'revoked'
                assert get()[0] == 200  # Older API response remains supported.
                Api.token_field = 'accessToken'
            assert good not in ''.join(logs) and KEY not in ''.join(logs)
            # Positive grants only: default 600s, one lookup per ID, all IIIF kinds.
            Api.calls.clear()
            cache_options = [x for x in options if 'anonymous_permission_ttl=' not in x]
            with server(executable, root, cache_options, variables=env) as (port, _):
                with ThreadPoolExecutor(max_workers=12) as pool:
                    results = list(pool.map(lambda _:get(),range(12)))
                    assert [status for status,_ in results] == [200]*12, results
                assert [m for m,_ in Api.calls].count('POST') == 1
                assert [m for m,_ in Api.calls].count('GET') == 1
                Api.mode = 'denied'  # Revocation is intentionally delayed while grant lives.
                for path in ('/asset/info.json', '/prefix/asset/info.json', '/asset'):
                    assert get(path=path)[0] == (303 if path=='/asset' else 200)
                assert get(method='HEAD')[0] == 200
                assert get('?token=wrong')[0] == 403
                assert [m for m,_ in Api.calls].count('GET') == 1
                assert get(path='/other/info.json')[0] == 403  # ID isolation.
                assert get(path='/other/info.json')[0] == 403  # Denials not retained.
                assert [m for m,_ in Api.calls].count('GET') == 3
            # Configured TTL expires without sliding on hits. Recheck even when
            # the rendered image itself remains in the persistent cache.
            Api.mode = 'allow'
            short_options = cache_options + ['--preflight-config=anonymous_permission_ttl=2']
            with server(executable, root, short_options, variables=env) as (port, _):
                assert get()[0] == 200
                before = len(Api.calls)
                time.sleep(1.1)
                Api.mode = 'broken'
                assert get()[0] == 200
                assert len(Api.calls) == before
                time.sleep(1.1)
                assert get()[0] == 500  # No stale allow on API failure.
                Api.mode = 'denied'
                assert get()[0] == 403
                Api.mode = 'restricted'
                assert get()[0] == 403
                Api.mode = 'unsafe'
                assert get()[0] == 403
                Api.mode = 'allow'
                Api.token = 'revoked'
                assert get()[0] == 200  # 401 renews outside permission loader.
                before = len(Api.calls)
                assert get()[0] == 200
                assert len(Api.calls) == before
                assert get('?token='+token(permval=0))[0] == 403
            # A new policy snapshot cannot inherit the old positive grant.
            Api.mode = 'denied'
            with server(executable, root, cache_options, variables=env) as (port, _):
                assert get()[0] == 403
            Api.mode = 'allow'
            # Invalid policy configuration fails at startup, before any API call.
            for invalid in ('-1','3601','1.5','not-a-number'):
                before = len(Api.calls)
                result = subprocess.run([str(executable), '--preflight-script',str(root/'policy.lua'),
                    '--preflight-config=minimum_permission=1',
                    '--preflight-config=anonymous_permission_ttl='+invalid],
                    env={**{k:v for k,v in os.environ.items() if not k.startswith('SIPI_')},**env},
                    capture_output=True, timeout=5)
                assert result.returncode != 0
                assert len(Api.calls) == before
            # No API configuration means anonymous access is disabled. Exercise the
            # explicitly selectable stricter permission policy in another startup.
            with server(executable, root, ['--preflight-config=minimum_permission=2'],
                        variables=env) as (port, _):
                assert get('?token='+token(permval=1))[0] == 403
                assert get('?token='+good)[0] == 200
                assert get()[0] == 403
            Api.user, Api.password = 'public-reader', 'fixture-password'
            custom_options = options + ['--preflight-config=unknown_user=public-reader']
            with server(executable, root, custom_options,
                        variables={**env, 'SIPI_PREFLIGHT_SECRETS_UNKNOWN_PASSWORD':Api.password}) as (port, _):
                assert get()[0] == 200
            # Short API lifetime proves real monotonic expiration across Lua states.
            Api.user, Api.password, Api.lifetime = 'unknown', '', 6
            with server(executable, root, options, variables=env) as (port, _):
                before = sum(method == 'POST' for method,_ in Api.calls)
                assert get()[0] == 200
                assert get()[0] == 200
                assert sum(method == 'POST' for method,_ in Api.calls) == before+1
                time.sleep(1.1)
                assert get()[0] == 200
                assert sum(method == 'POST' for method,_ in Api.calls) == before+2
            Api.lifetime = 0
            with server(executable, root, options, variables=env) as (port, _):
                before = sum(method == 'GET' for method,_ in Api.calls)
                assert get()[0] == 500
                assert sum(method == 'GET' for method,_ in Api.calls) == before
    finally:
        api.shutdown()
        thread.join()
        api.server_close()
    print('Shipped OLDAP policy: JWT, source binding, precedence, anonymous API and cache checks passed')


if __name__ == '__main__':
    main()
