"""Exercise decoded query credentials, request context and authorization before cache hits."""
from pathlib import Path
import base64
import hashlib
import hmac
import http.client
import json
import sys
import tempfile
import time
from urllib.parse import urlsplit
from cache_http import server
from png_metadata import fixture


def main():
    secret = 'test-only-secret-with-at-least-32-bytes'
    def segment(value):
        return base64.urlsafe_b64encode(json.dumps(value).encode()).rstrip(b'=').decode()
    payload = segment({'alg': 'HS256', 'typ': 'JWT'}) + '.' + segment({
        'sub': 'color.png', 'exp': int(time.time()) + 300})
    signature = base64.urlsafe_b64encode(hmac.new(secret.encode(), payload.encode(), hashlib.sha256).digest())
    token = payload + '.' + signature.rstrip(b'=').decode()
    with tempfile.TemporaryDirectory(prefix='sipi-context-') as temporary:
        root = Path(temporary)
        (root/'images').mkdir()
        (root/'images'/'color.png').write_bytes(fixture())
        (root/'policy.lua').write_text('''function preflight(r)
          local tokens = r.query.token
          if not tokens or #tokens ~= 1 then return {decision='deny'} end
          local claims = sipi.jwt.verify(tokens[1], {algorithm='HS256', key=sipi.secrets.jwt_key})
          if not claims or claims.sub ~= r.identifier then return {decision='deny'} end
          assert(r.kind == r.query.expected_kind[1])
          assert(r.method == r.query.expected_method[1])
          assert(#r.prefix == 2 and r.prefix[1] == 'project/part' and r.prefix[2] == 'sub')
          if r.query.check then
            assert(r.query.check[1] == 'a b+c%2F&=')
            assert(r.query.flag[1] == '' and r.query.empty[1] == '')
            assert(r.query.absent == nil)
          end
          return {decision='allow'}
        end''')
        extra = ['--public-base-url=https://images.example/iiif']
        with server(Path(sys.argv[1]).resolve(), root, extra,
                    variables={'SIPI_PREFLIGHT_SECRETS_JWT_KEY': secret}) as (port, log):
            def request(path, method='GET'):
                conn = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
                try:
                    conn.request(method, path)
                    response = conn.getresponse()
                    return response.status, dict((k.lower(), v) for k,v in response.getheaders()), response.read()
                finally:
                    conn.close()
            base = '/project%2Fpart/sub/color.png'
            image = base + '/full/max/0/default.png'
            def query(kind, method='GET'):
                return '?token=' + token + '&expected_kind=' + kind + '&expected_method=' + method
            for method in ('GET', 'HEAD'):
                for path, kind in ((image, 'image'), (base+'/info.json', 'info'), (base, 'base')):
                    url = path + query(kind, method)
                    for _ in range(2):
                        status, headers, body = request(url, method)
                        assert status == (303 if kind == 'base' else 200), (status, body)
                        assert headers['cache-control'] == 'no-store'
                        if method == 'HEAD':
                            assert not body
                        if kind == 'base':
                            assert headers['location'] == 'https://images.example/iiif' + base + '/info.json' + query(kind, method)
                            # A real policy need not carry expected_kind; adjust only our test assertion.
                            follow = urlsplit(headers['location'])
                            target = follow.path.removeprefix('/iiif') + '?' + follow.query.replace('expected_kind=base', 'expected_kind=info')
                            assert request(target, method)[0] == 200
                        elif kind == 'info' and method == 'GET':
                            assert '?' not in json.loads(body)['id']
                        assert token not in headers.get('link', '')
                    for suffix in ('', '?token=', '?token=wrong', query(kind, method)+'&token='+token,
                                   query(kind, method)+'&%74oken=wrong'):
                        assert request(path+suffix, method)[0] == 403
            assert request(image+query('image')+'&check=a+b%2Bc%252F%26%3D&flag&empty=')[0] == 200
            for bad in ('&x=%', '&x=%GG', '&x=%00', '&x=%0d%0a', '&=value', '&&x=y'):
                status, headers, body = request(image+query('image')+bad)
                assert status == 400 and headers['cache-control'] == 'no-store', (status, body)
                assert token.encode() not in body
            assert request(image+query('image')+'&x='+('v'*16385))[0] == 414
            assert request(image+query('image')+('&x=v'*128))[0] == 414
            assert request(base.replace('color.png', 'other.png')+query('base'))[0] == 403
        assert token not in ''.join(log), 'query credentials appeared in server logs'
    print('Preflight query/context, JWT, redirect and warm-cache authorization checks passed')


if __name__ == '__main__':
    main()
