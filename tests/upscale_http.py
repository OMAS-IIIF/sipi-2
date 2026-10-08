"""Exercise opt-in enlargement, advertised limits, canonical replay and cache policy."""
from pathlib import Path
import json
import sys
import tempfile
from cache_http import server, get
from png_metadata import fixture
from png_check import decode_png


def main():
    executable = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='sipi-upscale-') as temporary:
        root = Path(temporary)
        (root/'images').mkdir()
        (root/'images'/'image.png').write_bytes(fixture()) # 3 x 2 RGB.
        (root/'policy.lua').write_text("function preflight(r) if r.headers.authorization=='denied' then return {decision='deny'} end return {decision='allow'} end")
        limits = ['--max-width=12','--max-height=12','--max-area=96']
        with server(executable, root, limits + ['--size-upscaling=true']) as (port, log):
            info = json.loads(get(port,'/image.png/info.json')[2])
            assert 'sizeUpscaling' in info['extraFeatures']
            for size, dimensions in [('^6,',(6,4)),('^,4',(6,4)),('^pct:200',(6,4)),
                                     ('^6,1',(6,1)),('^1,4',(1,4)),('^!6,6',(6,4)),
                                     ('^max',(12,8)),('max',(3,2))]:
                path='/image.png/full/'+size+'/0/default.png'
                status,h,data=get(port,path)
                assert status==200,(path,status,data)
                assert decode_png(data)[:2]==dimensions
                assert get(port,path)[2]==data
                hs,hh,hb=get(port,path,'HEAD')
                assert hs==200 and not hb and dict((k.lower(),v) for k,v in hh.items())['content-length']==str(len(data))
                link=dict((k.lower(),v) for k,v in h.items())['link']
                canonical=link.split('>')[0][1:]
                from urllib.parse import urlsplit
                assert get(port,urlsplit(canonical).path)[2]==data
            assert get(port,'/image.png/full/6,/0/default.png')[0]==400
            assert get(port,'/image.png/full/^13,8/0/default.png')[0]==400
            assert get(port,'/image.png/full/^12,9/0/default.png')[0]==400
            assert get(port,'/image.png/full/^6,/0/default.png',authorization='denied')[0]==403
            assert decode_png(get(port,'/image.png/full/^6,/90/default.png')[2])[:2]==(4,6)
            assert get(port,'/image.png/full/^6,/0/default.jpg')[0]==200
            assert get(port,'/image.png/full/^6,/0/bitonal.png')[0]==200
        assert 'IIIF result cache hit' in log[0]
        # Existing enlarged entries cannot bypass disabled policy on restart.
        with server(executable, root, limits) as (port,_):
            assert 'sizeUpscaling' not in json.loads(get(port,'/image.png/info.json')[2])['extraFeatures']
            assert get(port,'/image.png/full/^6,/0/default.png')[0]==501
            assert get(port,'/image.png/full/^2,/0/default.png')[0]==200
        with server(executable, root, ['--size-upscaling=true','--max-width=4','--max-area=12']) as (port,_):
            assert get(port,'/image.png/full/^6,/0/default.png')[0]==400
    print('Upscaling HTTP and cache policy passed')


if __name__=='__main__':
    main()
