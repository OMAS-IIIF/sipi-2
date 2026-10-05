"""PNG pixel delivery, exact samples, transforms, authorization and caching."""
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import shutil
import sys
import tempfile
from cache_http import server, get, rows
from png_check import decode_png
from rotation_http import transformed_png, jpeg_size, bilinear_reference


def chunks(data):
    result = {}
    at = 8
    while at < len(data):
        size = int.from_bytes(data[at:at+4],'big')
        result[data[at+4:at+8]] = data[at+8:at+8+size]
        at += size+12
    return result


def without_icc(data):
    result = data[:2]
    at = 2
    while data[at+1] != 0xda:
        size = int.from_bytes(data[at+2:at+4],'big')+2
        if data[at+1] != 0xe2:
            result += data[at:at+size]
        at += size
    return result+data[at:]


def jpeg_profile(data):
    at = 2
    profile = bytearray()
    while data[at+1] != 0xda:
        size = int.from_bytes(data[at+2:at+4],'big')+2
        if data[at+1] == 0xe2:
            payload = data[at+4:at+size]
            assert payload[:14] == b'ICC_PROFILE\0\x01\x01'
            profile.extend(payload[14:])
        at += size
    return profile


def main():
    executable, fixtures = (Path(value).resolve() for value in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix='sipi-png-header-') as temporary:
        root = Path(temporary)
        images = root/'images'
        shutil.copytree(fixtures, images)
        shutil.copyfile(fixtures/'rgb.png', images/'opaque-id')
        (root/'policy.lua').write_text("""
function preflight(request)
  if request.headers.authorization == 'denied' then return {decision='deny'} end
  if request.identifier == 'alias' then return {decision='allow',source='rgba.png'} end
  return {decision='allow'}
end
""")
        with server(executable, root, ['--iiif-workers=4']) as (port, log):
            def verify(name):
                path = f'/museum/project/{name}/info.json'
                status, headers, body = get(port,path)
                assert status == 200, (name,body)
                info = json.loads(body)
                assert (info['width'],info['height']) == (3,2)
                assert info['id'].endswith('/museum/project/'+name)
                assert 'profile' not in info
                assert 'rotationBy90s' in info['extraFeatures']
                headers = {k.lower():v for k,v in headers.items()}
                assert headers['cache-control'] == 'no-store'
                status, head, body = get(port,path,'HEAD')
                assert status == 200 and not body
                assert {k.lower():v for k,v in head.items()}['content-length'] == headers['content-length']
                status, _, body = get(port,f'/{name}/full/max/0/default.png')
                if name == 'header-only.png':
                    assert status == 500
                else:
                    assert status == 200, (name,body)
                    reference = 'rgb' if name == 'opaque-id' else 'rgba' if name == 'alias' else name[:-4]
                    decoded = decode_png(body)
                    assert decoded[:2] == (3,2)
                    assert decoded[2] == (fixtures/(reference+'.raw')).read_bytes()
                    if name == 'icc.png':
                        assert decoded[3] == (fixtures/'expected.icc').read_bytes()
            names = ['rgb.png','gray.png','rgba.png','palette-alpha.png','icc.png',
                     'header-only.png','adam7.png','opaque-id','alias','gray-alpha.png','gray-low.png',
                     'rgb-trns.png','gray-trns.png','gray-icc.png','gray-alpha-icc.png','srgb.png','srgb-1.png',
                     'srgb-2.png','srgb-3.png','srgb-gray.png','srgb-gray-alpha.png',
                     'srgb-rgba.png','srgb-palette.png']
            with ThreadPoolExecutor(max_workers=8) as clients:
                list(clients.map(verify,names*3))
            for name,expected in [('sixteen',501),('exif',501),('broken',500),('bad-crc',500),('bad-icc',500)]:
                assert get(port,f'/{name}.png/info.json')[0] == expected
            assert get(port,'/rgb.png/info.json',authorization='denied')[0] == 403
            assert get(port,'/rgb.png/10,0,1,1/max/0/default.png')[0] == 400
            assert rows(root)
            before = len(rows(root))
            for name,expected in [('bad-idat',500),('no-iend',500),('late-exif',501),
                                  ('late-apng',501),('apng',501),('gamma',501)]:
                assert get(port,f'/{name}.png/0,0,1,1/max/0/default.png')[0] == expected
            assert len(rows(root)) == before
            assert get(port,'/rgb.png/full/max/0/default.png',authorization='denied')[0] == 403
            for name in ('rgb','gray','rgba','palette-alpha','gray-alpha','icc','adam7'):
                prefix = f'/{name}.png/1,0,2,2/1,1/'
                response = get(port,prefix+'0/default.png')
                assert response[0] == 200
                for mirror in (False,True):
                    for turn in range(4):
                        path = prefix+('!' if mirror else '')+str(turn*90)+'/default.png'
                        actual = get(port,path)
                        assert actual[0] == 200
                        assert decode_png(actual[2])[:4] == transformed_png(response[2],mirror,turn)
                        assert get(port,path)[2] == actual[2]
                jpeg = get(port,prefix+'0/default.jpg')
                assert jpeg[0] == 200 and jpeg_size(jpeg[2]) == (1,1)
            # Explicit sRGB survives processing; ICC assignment changes JPEG
            # metadata only, never its encoded pixel stream.
            for name,intent,gray in [('srgb',0,False),('srgb-1',1,False),('srgb-2',2,False),
                    ('srgb-3',3,False),('srgb-gray',2,True),('srgb-gray-alpha',2,True),
                    ('srgb-rgba',2,False),('srgb-palette',2,False)]:
                for operation in ('full/max/0','full/2,1/!90','1,0,2,2/1,1/!90','full/max/37.5'):
                    path = f'/{name}.png/{operation}/default.'
                    png = get(port,path+'png')[2]
                    assert chunks(png)[b'sRGB'] == bytes([intent])
                    assert b'iCCP' not in chunks(png)
                    jpeg = get(port,path+'jpg')[2]
                    profile = jpeg_profile(jpeg)
                    template = 'sGrey-v4.icc' if gray else 'sRGB-v4.icc'
                    expected = bytearray((Path(__file__).resolve().parents[1]/'third_party/compact-icc'/template).read_bytes())
                    expected[64:68] = intent.to_bytes(4,'big')
                    expected[84:100] = bytes(16)
                    assert profile == expected
                base = {'srgb-gray':'gray','srgb-gray-alpha':'gray-alpha','srgb-rgba':'rgba',
                        'srgb-palette':'palette-alpha'}.get(name,'rgb')
                tagged = get(port,f'/{name}.png/full/max/0/default.jpg')[2]
                plain = get(port,f'/{base}.png/full/max/0/default.jpg')[2]
                assert without_icc(tagged) == plain
            for name in ('gray-icc','icc','srgb-gray','srgb-rgba'):
                base=get(port,f'/{name}.png/full/max/0/default.png')[2]
                rotated_profile=get(port,f'/{name}.png/full/max/!37.5/default.png')
                assert rotated_profile[0]==200
                actual=decode_png(rotated_profile[2])
                expected=bilinear_reference(base,37.5,True)
                assert actual[:2]==expected[:2] and actual[3]==expected[3]
                assert max(abs(a-b) for a,b in zip(actual[2],expected[2]))<=1
            # Non-square Adam7 crop/rotation checks dimensions and ordering.
            prefix = '/adam7-large.png/3,2,8,6/4,3/'
            original = get(port,prefix+'0/default.png')[2]
            path = prefix+'!90/default.png'
            rotated = get(port,path)[2]
            assert decode_png(rotated)[:4] == transformed_png(original,True,1)
            assert get(port,path,'HEAD')[0] == 200
            # Replacing a cached valid header with a corrupt source must invalidate it.
            shutil.copyfile(fixtures/'broken.png', images/'replacement')
            (images/'replacement').replace(images/'opaque-id')
            assert get(port,'/opaque-id/info.json')[0] == 500
            (images/'rgb.png').unlink()
            assert get(port,'/rgb.png/info.json')[0] == 404
        assert 'IIIF metadata cache hit' in log[0]
        assert 'IIIF result cache stored' in log[0]
        with server(executable,root) as (port,restarted):
            assert get(port,path)[2] == rotated
        assert 'IIIF result cache hit' in restarted[0]
    print('PNG info/HEAD, policies, metadata cache and capability boundary passed')


if __name__ == '__main__':
    main()
