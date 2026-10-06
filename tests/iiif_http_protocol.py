"""HTTP requirements: negotiation, authorized redirects, CORS and level2 discovery."""
from pathlib import Path
import http.client
import json
import sys
import tempfile
from cache_http import server
from png_metadata import fixture


def main():
    executable=Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='sipi-http3-') as temporary:
        root=Path(temporary)
        (root/'images').mkdir()
        (root/'images'/'color.png').write_bytes(fixture())
        (root/'images'/'gray.png').write_bytes(fixture(color=0))
        (root/'policy.lua').write_text("""function preflight(r)
          if r.headers.authorization=='denied' then return {decision='deny'} end
          if r.identifier=='alias' then return {decision='allow',source='color.png'} end
          return {decision='allow'}
        end""")
        with server(executable,root,['--public-base-url=https://images.example/iiif']) as (port,_):
            def request(path,method='GET',headers=None):
                conn=http.client.HTTPConnection('127.0.0.1',port,timeout=10)
                try:
                    conn.request(method,path,headers=headers or {})
                    r=conn.getresponse()
                    result=(r.status,{k.lower():v for k,v in r.getheaders()},r.read())
                    assert result[1].get('access-control-allow-origin')=='*'
                    assert 'access-control-allow-credentials' not in result[1]
                    return result
                finally:
                    conn.close()
            for path in ('/color.png','/project/sub/alias','/a/b/c/d/e/alias','/a%2Fb/alias'):
                status,headers,body=request(path,headers={'Host':'untrusted.example'})
                assert status==303, (path,status,body)
                assert headers['location']=='https://images.example/iiif'+path+'/info.json'
                assert request(path,'HEAD')[0]==303
                assert request(path,headers={'Authorization':'denied'})[0]==403
            assert request('/missing')[0]==404
            path='/color.png/info.json'
            status,headers,body=request(path)
            assert status==200 and headers['content-type'].startswith('application/ld+json')
            assert 'profile="http://iiif.io/api/image/3/context.json"' in headers['content-type']
            info=json.loads(body)
            assert info['profile']=='level2'
            assert info['extraQualities']==['color','gray','bitonal']
            assert 'rotationBy90s' not in info['extraFeatures'] # Supplied by level2.
            assert json.loads(request('/gray.png/info.json')[2])['extraQualities']==['gray','bitonal']
            for accept,media in (
                ('application/json','application/json'),
                ('application/ld+json','application/ld+json'),
                ('application/ld+json;q=0,*/*;q=1','application/json'),
                ('application/json;q=0.9,application/ld+json;q=0.1','application/json'),
                ('application/ld+json;profile="http://iiif.io/api/image/3/context.json"','application/ld+json'),
                ('application/json;charset=utf-8','application/json'),
            ):
                status,headers,data=request(path,headers={'Accept':accept})
                assert status==200 and headers['content-type'].startswith(media)
                assert headers['vary']=='Accept' and json.loads(data)==info
                hs,hh,hb=request(path,'HEAD',{'Accept':accept})
                assert hs==200 and not hb and hh['content-length']==str(len(data))
            for accept in ('text/html','application/json;q=0,application/ld+json;q=0','application/ld+json;profile="other"'):
                status,headers,_=request(path,headers={'Accept':accept})
                assert status==406 and headers['vary']=='Accept'
            for target in (path,'/color.png/full/max/0/default.png','/color.png/full/max/0/default.jpg'):
                for _ in range(2): # Images include a cache hit.
                    status,headers,_=request(target)
                    assert status==200 and '<http://iiif.io/api/image/3/level2.json>;rel="profile"' in headers['link']
            # Canonical links are recomputed for public aliases/prefixes even on cache hits.
            for identity in ('color.png', 'project/alias', 'a%2Fb/alias'):
                for suffix,canonical_suffix in (
                    ('pct:0,0,100,100/pct:100/360/color.png','full/max/0/default.png'),
                    ('full/1,1/!090.000/default.png','full/1,1/!90/default.png'),
                    ('full/max/12.3400/default.jpg','full/max/12.34/default.jpg')):
                    target='/'+identity+'/'+suffix
                    canonical='https://images.example/iiif/'+identity+'/'+canonical_suffix
                    for method in ('GET','GET','HEAD'):
                        status,h,data=request(target,method,{'Host':'untrusted.example'})
                        assert status==200 and 'location' not in h
                        assert '<'+canonical+'>;rel="canonical"' in h['link'], h
                        if method=='GET':
                            replay=request('/'+identity+'/'+canonical_suffix)
                            assert replay[0]==200 and replay[2]==data
                        else:
                            assert not data
            assert 'rel="canonical"' not in request('/missing')[1].get('link','')
            assert 'rel="canonical"' not in request(path)[1].get('link','')
            assert request('/color.png/full/max/0/default.tif')[0]==400
            assert request('/color.png/no-region/max/0/default.png')[0]==400
            # OPTIONS is policy discovery, never Lua authorization or source lookup.
            status,headers,body=request('/missing','OPTIONS',{
                'Origin':'https://viewer.example','Access-Control-Request-Method':'GET',
                'Access-Control-Request-Headers':'authorization,accept','Authorization':'denied'})
            assert status==204 and not body and 'Authorization' in headers['access-control-allow-headers']
            assert request(path,'OPTIONS',{'Access-Control-Request-Method':'DELETE'})[0]==403
            assert request(path,'OPTIONS',{'Access-Control-Request-Method':'GET',
                                         'Access-Control-Request-Headers':'X-Private'})[0]==403
            assert request(path,'POST')[0]==405
            assert request(path,headers={'Authorization':'denied'})[0]==403
        # max uses the active limits, not necessarily the original dimensions.
        with server(executable,root,['--max-width=2','--max-height=2','--max-area=4']) as (port,_):
            for target in ('/color.png/full/2,1/0/color.png', '/gray.png/full/2,1/0/gray.png'):
                conn=http.client.HTTPConnection('127.0.0.1',port,timeout=10)
                conn.request('GET',target)
                r=conn.getresponse()
                assert r.status==200
                assert '/full/max/0/default.png>;rel="canonical"' in r.getheader('Link')
                r.read()
                conn.close()
        for credentials in ('false', 'true'):
            with server(executable,root,[
                '--cors-origins=https://viewer.example,https://other.example',
                '--cors-credentials='+credentials]) as (port,_):
                for origin in (None,'https://viewer.example','https://other.example',
                               'https://evil.example','null','https://viewer.example.evil'):
                    for path,method in (('/color.png/info.json','GET'),
                                        ('/color.png/full/max/0/default.png','GET'),
                                        ('/color.png','GET'),('/missing','GET'),
                                        ('/color.png/info.json','OPTIONS')):
                        conn=http.client.HTTPConnection('127.0.0.1',port,timeout=10)
                        headers={'Origin':origin} if origin is not None else {}
                        headers['Access-Control-Request-Method']='GET'
                        headers['Access-Control-Request-Headers']='authorization'
                        conn.request(method,path,headers=headers)
                        response=conn.getresponse()
                        h={k.lower():v for k,v in response.getheaders()}
                        response.read()
                        conn.close()
                        assert 'Origin' in h['vary']
                        if path.endswith('info.json') and method=='GET':
                            assert 'Accept' in h['vary']
                        if origin in ('https://viewer.example','https://other.example'):
                            assert h.get('access-control-allow-origin')==origin
                            assert h.get('access-control-allow-credentials')==('true' if credentials=='true' else None)
                        else:
                            assert 'access-control-allow-origin' not in h
                            assert 'access-control-allow-credentials' not in h
    print('IIIF HTTP negotiation, CORS, redirects, profile and cache headers passed')


if __name__=='__main__':
    main()
