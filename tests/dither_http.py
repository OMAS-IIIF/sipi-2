"""Independent full-raster diffusion oracle and cache separation across policies."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import sys
import tempfile
from cache_http import server, get, rows
from png_metadata import fixture
from png_check import decode_png


def reference(width, height, samples):
    """Diffuse directly into a full float raster, with explicit boundary checks."""
    values=list(map(float,samples))
    output=bytearray(width*height)
    for y in range(height):
        direction=1 if y%2==0 else -1
        for x in (range(width) if direction==1 else range(width-1,-1,-1)):
            i=y*width+x
            output[i]=0 if values[i]<128 else 255
            error=values[i]-output[i]
            for dx,dy,weight in ((direction,0,7),(-direction,1,3),(0,1,5),(direction,1,1)):
                nx,ny=x+dx,y+dy
                if 0<=nx<width and ny<height:
                    values[ny*width+nx]+=error*weight/16
    return bytes(output)


def main():
    executable=Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='sipi-dither-') as temporary:
        root=Path(temporary)
        (root/'images').mkdir()
        (root/'images'/'gray.png').write_bytes(fixture(color=0,width=37,height=23))
        (root/'images'/'alpha.png').write_bytes(fixture(color=4,width=37,height=23))
        (root/'policy.lua').write_text("function preflight(r) if r.headers.authorization=='denied' then return {decision='deny'} end return {decision='allow'} end")
        paths=[f'/{name}.png/full/max/{angle}/bitonal.png'
               for name in ('gray','alpha') for angle in ('0','!37.5')]
        with server(executable,root) as (port,_):
            old={path:get(port,path)[2] for path in paths}
            gray_saved=get(port,'/gray.png/full/max/0/gray.png')[2]
        count=len(rows(root))
        expected={}
        with server(executable,root,variables={'SIPI_BITONAL_METHOD':'floyd-steinberg'}) as (port,_):
            def check(path):
                gray=decode_png(get(port,path.replace('/bitonal.png','/gray.png'))[2])
                w,h,samples,profile,_=gray
                if len(samples)==w*h*2:
                    samples=bytes((samples[i]*samples[i+1]+255*(255-samples[i+1])+127)//255
                                  for i in range(0,len(samples),2))
                status,_,data=get(port,path)
                assert status==200
                assert data[24:26]==bytes([1,0])
                assert decode_png(data)[:4]==(w,h,reference(w,h,samples),profile)
                assert data!=old[path]
                assert get(port,path[:-3]+'jpg')[0]==200
                return data
            with ThreadPoolExecutor(max_workers=8) as clients:
                outputs=list(clients.map(check,paths*2))
            assert outputs[:4]==outputs[4:]
            expected=dict(zip(paths,outputs))
            assert get(port,paths[0],authorization='denied')[0]==403
            assert get(port,paths[0],'HEAD')[0]==200
            assert get(port,'/gray.png/full/max/0/gray.png')[2]==gray_saved
        assert len(rows(root))>count
        with server(executable,root,['--bitonal-method=floyd-steinberg']) as (port,log):
            for path,data in expected.items():
                assert get(port,path)[2]==data
        assert 'IIIF result cache hit' in log[0]
        with server(executable,root,['--bitonal-method=floyd-steinberg'],cache=None) as (port,_):
            for path,data in expected.items():
                assert get(port,path)[2]==data
        # Switching back must reuse the original threshold entries, not dithered ones.
        with server(executable,root) as (port,log):
            for path,data in old.items():
                assert get(port,path)[2]==data
        assert 'IIIF result cache hit' in log[0]
    print('Dither oracle, rotation/alpha, concurrency and policy cache separation passed')


if __name__=='__main__':
    main()
