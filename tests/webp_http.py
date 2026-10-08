"""WebP HTTP, pixel decoding, restart/cache, policies and format limits."""
from pathlib import Path
import json
import sqlite3
import struct
import subprocess
import sys
import tempfile
from cache_http import server, get
from png_metadata import fixture


def chunks(data):
    assert data[:4]==b'RIFF' and data[8:12]==b'WEBP'
    assert struct.unpack_from('<I',data,4)[0]+8==len(data)
    result={}
    offset=12
    while offset<len(data):
        name=data[offset:offset+4]
        size=struct.unpack_from('<I',data,offset+4)[0]
        result[name]=data[offset+8:offset+8+size]
        offset+=8+size+(size%2)
    assert offset==len(data)
    return result


def main():
    executable, verifier=map(lambda p:Path(p).resolve(),sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='sipi-webp-') as temporary:
        root=Path(temporary)
        (root/'images').mkdir()
        for name,color in [('rgb',2),('rgba',6),('gray',0),('ga',4)]:
            (root/'images'/(name+'.png')).write_bytes(fixture(width=20,height=12,color=color))
        (root/'images'/'wide.png').write_bytes(fixture(width=17000,height=1,color=0))
        (root/'policy.lua').write_text("function preflight(r) if r.headers.authorization=='denied' then return {decision='deny'} end return {decision='allow'} end")
        saved={}
        for iteration in range(2):
            with server(executable,root,['--size-upscaling=true']) as (port,log):
                assert json.loads(get(port,'/rgb.png/info.json')[2])['extraFormats']==['webp']
                for name in ('rgb','rgba','gray','ga'):
                    for quality in ('default','gray','bitonal'):
                        path=f'/{name}.png/2,2,10,6/^20,12/!90/{quality}.webp'
                        status,headers,data=get(port,path)
                        assert status==200,(path,status,data)
                        h={k.lower():v for k,v in headers.items()}
                        assert h['content-type'].startswith('image/webp') and h['cache-control']=='no-store'
                        assert '.webp>;rel="canonical"' in h['link']
                        content=chunks(data)
                        lossless=name in ('gray','ga') or quality!='default'
                        assert (b'VP8L' in content)==lossless
                        if lossless:
                            assert content[b'ICCP'][16:20]==b'RGB '
                        output=root/'output.webp'
                        output.write_bytes(data)
                        subprocess.run([str(verifier),str(output),'12','20',
                                        'binary' if quality=='bitonal' else 'normal'],check=True)
                        hs,hh,hb=get(port,path,'HEAD')
                        assert hs==200 and not hb
                        assert {k.lower():v for k,v in hh.items()}['content-length']==str(len(data))
                        assert get(port,path,authorization='denied')[0]==403
                        if iteration: assert data==saved[path]
                        else: saved[path]=data
                png=get(port,'/rgb.png/full/max/0/default.png')[2]
                webp=get(port,'/rgb.png/full/max/0/default.webp')[2]
                assert png!=webp and get(port,'/rgb.png/full/max/0/default.webp')[2]==webp
            assert 'IIIF result cache hit' in log[0]
        with sqlite3.connect(root/'cache'/'cache.sqlite3') as db:
            assert db.execute("SELECT count(*) FROM entries WHERE media=?",(b'image/webp',)).fetchone()[0]>0
        with server(executable,root,['--max-width=20000','--max-height=2','--max-area=20000']) as (port,_):
            assert get(port,'/wide.png/full/max/0/default.webp')[0]==400
            assert get(port,'/wide.png/full/max/0/default.png')[0]==200
            assert get(port,'/wide.png/full/1000,1/0/default.webp')[0]==200
    print('WebP HTTP, decoder, cache restart and limits passed')


if __name__=='__main__':
    main()
