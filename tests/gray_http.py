"""Gray/bitonal delivery and cache policy, including invalid-profile failures."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import shutil
import sys
import tempfile
import zlib
from cache_http import server, get, rows
from png_metadata import fixture, chunk
from png_check import decode_png
from png_http import jpeg_profile


def main():
    exe, fixtures, profiles = (Path(p).resolve() for p in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='sipi-gray-') as temporary:
        root=Path(temporary)
        shutil.copytree(fixtures,root/'images')
        (root/'images'/'linear.png').write_bytes(fixture(extra=chunk(b'iCCP',b'Linear\0\0'+zlib.compress((profiles/'linear-rgb.icc').read_bytes()))))
        (root/'policy.lua').write_text("function preflight(r) if r.headers.authorization=='denied' then return {decision='deny'} end return {decision='allow'} end")
        path='/rgba.png/full/max/!37.5/bitonal.png'
        with server(exe,root,['--iiif-workers=4']) as (port,log):
            def render(name):
                url=f'/{name}.png/full/max/0/gray.png'
                status,_,data=get(port,url)
                assert status==200,(name,data)
                w,h,pixels,profile,_=decode_png(data)
                assert (w,h)==(3,2) and len(pixels)==(12 if name=='rgba' else 6)
                assert profile[16:20]==b'GRAY' and profile[64:68]==b'\0\0\0\1'
                assert get(port,url)[2]==data
                jpeg=get(port,url[:-3]+'jpg')
                assert jpeg[0]==200 and jpeg_profile(jpeg[2])==profile
                return pixels,profile
            with ThreadPoolExecutor(max_workers=8) as clients:
                results=list(clients.map(render,['rgb','srgb','linear','rgba']*3))
            assert results[0]==results[1] # Truly untagged RGB assumes sRGB only here.
            assert results[0][0]!=results[2][0] # Embedded linear RGB is actually used.
            assert results[3][0][1::2]==(fixtures/'rgba.raw').read_bytes()[3::4]
            # Native gray/gray-alpha keeps samples and original profile, or remains untagged.
            for name in ('gray','gray-icc','gray-alpha','gray-alpha-icc','srgb-gray'):
                original=get(port,f'/{name}.png/full/max/0/default.png')
                gray=get(port,f'/{name}.png/full/max/0/gray.png')
                assert original[0]==gray[0]==200 and original[2]==gray[2]
            # Derive the threshold oracle independently from the delivered gray samples.
            for name in ('rgb','srgb','linear','rgba','gray','gray-icc','gray-alpha','gray-alpha-icc','srgb-gray'):
                for angle in ('0','!37.5'):
                    base=f'/{name}.png/full/max/{angle}/'
                    gray=decode_png(get(port,base+'gray.png')[2])
                    w,h,samples,profile,_=gray
                    channels=len(samples)//(w*h)
                    assert channels in (1,2)
                    values=samples if channels==1 else bytes(
                        (samples[i]*samples[i+1]+255*(255-samples[i+1])+127)//255
                        for i in range(0,len(samples),2))
                    expected=bytes(0 if v<128 else 255 for v in values)
                    response=get(port,base+'bitonal.png')
                    assert response[0]==200
                    assert response[2][24:26]==bytes([1,0])
                    bw=decode_png(response[2])
                    assert bw[:4]==(w,h,expected,profile),(name,angle)
                    assert get(port,base+'bitonal.png')[2]==response[2]
                    jpeg=get(port,base+'bitonal.jpg')
                    assert jpeg[0]==200
                    # Explicit native sRGB is assigned a gray ICC by the JPEG writer.
                    if name!='srgb-gray':
                        assert jpeg_profile(jpeg[2])==(profile or b'')
            before=len(rows(root))
            for name,status in [('icc',500),('gamma',501)]:
                for quality in ('gray','bitonal'):
                    actual=get(port,f'/{name}.png/full/max/0/{quality}.png')
                    assert actual[0]==status
            assert len(rows(root))==before # Unusable ICC never falls back or gets published.
            result=get(port,path)
            assert result[0]==200
            saved=result[2]
            gray_path=path.replace('/bitonal.png','/gray.png')
            saved_gray=get(port,gray_path)[2]
            assert get(port,path,authorization='denied')[0]==403
            status,headers,body=get(port,path,'HEAD')
            assert status==200 and not body
            assert int({k.lower():v for k,v in headers.items()}['content-length'])==len(saved)
        assert 'Gray conversion assumes sRGB' in log[0]
        with server(exe,root) as (port,restart):
            assert get(port,path)[2]==saved
            assert get(port,gray_path)[2]==saved_gray
        assert 'IIIF result cache hit' in restart[0]
        # A fresh, uncached process produces identical pixels AND profile bytes.
        with server(exe,root,cache=None) as (port,_):
            assert get(port,path)[2]==saved
            assert get(port,gray_path)[2]==saved_gray
    print('Gray/bitonal conversion, alpha, ICC policy, restart and fresh determinism passed')


if __name__=='__main__':
    main()
