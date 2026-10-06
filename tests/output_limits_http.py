"""Shared info/image limits, source admission, and cache checks after reconfiguration."""
from pathlib import Path
import json
import struct
import sys
import tempfile
import zlib
from cache_http import server, get
from png_metadata import fixture, chunk
from png_check import decode_png


def main():
    executable=Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='sipi-limits-') as temporary:
        root=Path(temporary)
        (root/'images').mkdir()
        (root/'policy.lua').write_text("function preflight(r) return {decision='allow'} end")
        (root/'images'/'source.png').write_bytes(fixture(width=320,height=200))
        (root/'images'/'wide.png').write_bytes(fixture(color=0,width=70000,height=1))
        # Header alone is enough to reject an impossible full-decode working set.
        header=chunk(b'IHDR',struct.pack('>IIBBBBB',100000,100000,8,2,0,0,0))
        (root/'images'/'huge.png').write_bytes(b'\x89PNG\r\n\x1a\n'+header+
            chunk(b'IDAT',zlib.compress(b''))+chunk(b'IEND',b''))
        tags=[(256,4,2),(257,4,3),(258,3,8),(259,3,1),(262,3,1),
              (273,4,146),(274,3,6),(277,3,1),(278,4,3),(279,4,6),(284,3,1)]
        data=b'II'+struct.pack('<HIH',42,8,len(tags))
        data+=b''.join(struct.pack('<HHII',tag,kind,1,value) for tag,kind,value in tags)
        (root/'images'/'oriented.tif').write_bytes(data+struct.pack('<I',0)+bytes(6))
        path='/source.png/full/200,125/0/default.png'
        with server(executable,root) as (port,_):
            assert get(port,path)[0]==200 # Populate cache above the later policy limit.
            info=json.loads(get(port,'/wide.png/info.json')[2])
            assert (info['width'],info['height'])==(70000,1)
            assert (info['maxWidth'],info['maxHeight'],info['maxArea'])==(8192,8192,8388608)
            assert get(port,'/wide.png/full/max/0/default.jpg')[0]==200
            assert decode_png(get(port,'/wide.png/full/max/0/default.png')[2])[:2]==(8192,1)
            for name in ('huge.png','oriented.tif'):
                for suffix in ('info.json','full/max/0/default.png'):
                    status,_,body=get(port,f'/{name}/{suffix}')
                    assert status==501 and json.loads(body)['error']=='source_not_deliverable'
                    assert get(port,f'/{name}/{suffix}','HEAD')[0]==501
        (root/'limits.toml').write_text('max-width=160\nmax-area=10000\n')
        with server(executable,root,['--config=limits.toml']) as (port,_):
            info=json.loads(get(port,'/source.png/info.json')[2])
            assert (info['width'],info['height'])==(320,200)
            assert (info['maxWidth'],info['maxHeight'],info['maxArea'])==(160,160,10000)
            assert get(port,path)[0]==400 # Existing cached bytes cannot bypass new policy.
            for size in ('max','!300,300'):
                response=get(port,f'/source.png/full/{size}/0/default.png')
                assert response[0]==200
                assert decode_png(response[2])[:2]==(126,79)
            for size in ('161,1','1,161','101,100'):
                assert get(port,f'/source.png/full/{size}/0/default.png')[0]==400
            assert get(port,'/source.png/full/100,100/0/default.png')[0]==200
            assert decode_png(get(port,'/source.png/0,0,40,30/max/0/default.png')[2])[:2]==(40,30)
        # Per-field precedence; unset height inherits the final effective width.
        with server(executable,root,['--config=limits.toml','--max-width=120'],
                    variables={'SIPI_MAX_WIDTH':'140','SIPI_MAX_AREA':'8000'}) as (port,_):
            info=json.loads(get(port,'/source.png/info.json')[2])
            assert (info['maxWidth'],info['maxHeight'],info['maxArea'])==(120,120,8000)
    print('Source admission, advertised limits, wide JPEG and cache-policy enforcement passed')


if __name__=='__main__':
    main()
