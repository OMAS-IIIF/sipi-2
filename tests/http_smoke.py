"""Exercise the real HTTP listener, URL escaping, routing, and graceful shutdown.

Uses only Python's standard library. Starts the executable in an isolated
working directory on a free loopback port and always reaps the child process.
"""

from png_check import decode_png
import concurrent.futures
import html
import http.client
import json
import os
from pathlib import Path
import signal
import shutil
import socket
import subprocess
import sys
import tempfile
import time


def alpha_pixel(x, y, associated=False):
    p = ((0,0,255,0), (255,0,0,255), (90,180,30,85), (255,0,255,1), (64,128,192,128))[(x+y)%5]
    if associated:
        a = p[3]
        return tuple((((c*a+127)//255)*255+a//2)//a if a else 0 for c in p[:3]) + (a,)
    return p


def alpha_reference(region, output, level=(35,19), associated=False):
    """Exact rational box integration in original full-image coordinates."""
    from fractions import Fraction as F
    from math import floor, ceil
    rx,ry,rw,rh = region
    ow,oh = output
    dx,dy = F(rw*level[0],35*ow), F(rh*level[1],19*oh)
    result = bytearray()
    for y in range(oh):
        top = F(ry*level[1],19)+y*dy
        for x in range(ow):
            left = F(rx*level[0],35)+x*dx
            colors, alpha = [F(0)]*3, F(0)
            for sy in range(floor(top),ceil(top+dy)):
                for sx in range(floor(left),ceil(left+dx)):
                    weight = (min(top+dy,sy+1)-max(top,sy))*(min(left+dx,sx+1)-max(left,sx))
                    pixel = alpha_pixel(sx,sy,associated)
                    alpha += weight*pixel[3]
                    for c in range(3): colors[c] += weight*pixel[c]*pixel[3]
            a = floor(alpha/(dx*dy)+F(1,2))
            result.extend([floor(c/alpha+F(1,2)) if a else 0 for c in colors]+[a])
    return bytes(result)


def run(executable: Path, fixtures: Path, verifier: Path, configured: bool = False, upscaling: bool = False) -> None:
    """Run HTTP assertions; raise on failure and clean up the server in all cases."""
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    version = subprocess.check_output([str(executable), "--version"], text=True).strip().split()[-1]
    env = {key: value for key, value in os.environ.items() if not key.startswith("SIPI_")}
    with tempfile.TemporaryDirectory(prefix="sipi-http-") as directory:
        root = Path(directory, "images")
        root.mkdir()
        for name in ("source.jph", "source.jhc", "offset.jph", "source.jp2", "source.j2k", "offset.jp2", "metadata-box.jp2", "corrupt-tile.jp2", "sixteen.jp2", "rgb-unknown.j2k", "sample.tif", "rotated.tif", "large.tif", "tiled.tif", "broken-tile.tif", "pyramid.tif", "pyramid-level.tif", "pyramid-stripped.tif", "invalid-profile.tif", "alpha.tif", "alpha-associated.tif", "alpha-pyramid.tif", "alpha-unspecified.tif", "alpha-invalid.tif", "gray.tif", "gray-white.tif", "gray-tiled.tif", "gray-pyramid.tif", "strip-rgba.tif"):
            shutil.copyfile(fixtures / name, root / name)
        for name in ("id", "hello-world", "a/b", "a%2Fb", "a+b", "café.tif", "source with spaces.tif"):
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(fixtures / "sample.tif", target)
        (root / "broken.jp2").write_bytes((fixtures / "source.jp2").read_bytes()[:32])
        (root / "broken.tif").write_bytes(b"II*\x00broken")
        (root / "other.png").write_bytes(b"\x89PNG\r\n\x1a\n")
        (root / "config").mkdir()
        (root / "outside-link").symlink_to(Path(directory))
        (root / "inside-link").symlink_to(root / "sample.tif")
        arguments = [str(executable), "--port", str(port), "--iiif-root", "images",
                     "--size-upscaling", str(upscaling).lower()]
        public_base = "https://images.example.org/iiif" if configured else f"http://127.0.0.1:{port}"
        if configured:
            env["SIPI_PUBLIC_BASE_URL"] = public_base + "/"
            # Relative paths use cwd, not the TOML file's directory. Initialization
            # runs at startup, and requests are independently authorized.
            Path(directory, "policy with spaces.lua").write_text(
                """assert(request == nil)
function preflight(request)
    local id = request.identifier
    if id == 'jpeg-alias' then return {decision='allow', source='tiled.tif'} end
    if id == 'source.jph' or id == 'source.jhc' or id == 'offset.jph' or id == 'metadata-box.jp2' or id == 'corrupt-tile.jp2' or id == 'sixteen.jp2' or id == 'rgb-unknown.j2k' or id == 'source.jp2' or id == 'source.j2k' or id == 'offset.jp2' or id == 'broken.jp2' or id == 'tiled.tif' or id == 'broken-tile.tif' or id == 'invalid-profile.tif' or id:match('^pyramid') or id:match('^alpha') or id:match('^gray') or id == 'strip-rgba.tif' then return {decision='allow', source=id} end
    if id == 'denied' then return {decision='deny'} end
    if id == 'script-error' then error(sipi.secrets.test_token .. ' /private/source.tif') end
    if id == 'invalid-result' then return {decision='allow', restrictions={}} end
    if id == 'loop' then while true do end end
    if id == 'memory' then local huge = string.rep('x', 32*1024*1024) end
    if id == 'missing-source' then return {decision='allow', source='missing.tif'} end
    if id == 'directory-source' then return {decision='allow', source='config'} end
    if id == 'no-source' then return {decision='allow'} end
    if id == 'absolute' then return {decision='allow', source=request.headers.authorization} end
    if id == 'large.tif' or id == 'use-large' then return {decision='allow', source='large.tif'} end
    if id == 'sample.tif' or id == 'rotated.tif' or id == 'broken.tif' or id == 'other.png' or
       id == 'inside-link' or id == 'outside-link/policy with spaces.lua' or id == '../policy with spaces.lua' then
        return {decision='allow', source=id}
    end
    if id == 'credentials' then
        if request.headers.authorization ~= 'Bearer test-token' or request.cookies.session ~= 'abc' then
            return {decision='deny'}
        end
    end
    if id == 'encoded/id' then
        assert(#request.prefix == 1 and request.prefix[1] == 'project/part')
    end
    return {decision='allow', source='source with spaces.tif'}
end
""")
            Path(directory, "config").mkdir()
            Path(directory, "config", "server.toml").write_text(
                'preflight-script = "policy with spaces.lua"\n')
            arguments += ["--config", "config/server.toml", "--preflight-secret", "test_token=secret-token"]
        with tempfile.TemporaryFile(mode="w+") as log:
            process = subprocess.Popen(
                arguments, cwd=directory,
                env=env, stdout=log, stderr=subprocess.STDOUT,
            )
            try:
                deadline = time.monotonic() + 10
                while True:
                    if process.poll() is not None:
                        raise AssertionError("Server exited before accepting connections")
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                            break
                    except OSError:
                        if time.monotonic() > deadline:
                            raise AssertionError("Server startup timed out")
                        time.sleep(0.05)

                def request(path: str, method: str = "GET", headers=None):
                    """Read one complete HTTP response with a bounded timeout."""
                    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=3)
                    try:
                        connection.request(method, path, headers=headers or {})
                        response = connection.getresponse()
                        body = response.read()
                        if not response.getheader("Content-Type", "").startswith("image/"):
                            body = body.decode()
                        return response.status, dict(response.getheaders()), body
                    finally:
                        connection.close()

                for path in ("/hello", "/hello/"):
                    status, headers, body = request(path)
                    assert status == 200 and "text/html" in headers.get("content-type", ""), (status, headers)
                    assert "<p>Hello World</p>" in body, body
                for path in ("/hello/id", "/hello/path1/path2/id", "/hello/a%20b?id=42&name=test"):
                    status, _, body = request(path)
                    expected = f"You called me with the URL http://127.0.0.1:{port}{path}"
                    assert status == 200 and expected in html.unescape(body), body
                status, _, body = request('/hello/id?q=<script>&x="value"')
                assert status == 200 and "<script>" not in body, body
                assert "&lt;script&gt;" in body and "&amp;" in body and "&quot;" in body, body
                status, _, body = request("/hello/id", headers={"Host": "example.test:9090"})
                assert status == 200 and "http://example.test:9090/hello/id" in body, body
                status, _, body = request("/hello/id", headers={"X-Forwarded-Proto": "https"})
                assert status == 200 and "http://127.0.0.1:" in body, body
                def json_request(path: str, expected_status: int, method: str = "GET"):
                    """Check HTTP status/content type before returning parsed JSON."""
                    status, headers, body = request(path, method)
                    assert status == expected_status, (path, method, status, body[:300])
                    assert any(t in headers.get("content-type", "") for t in ("application/json", "application/ld+json")), headers
                    return headers, json.loads(body)

                _, health = json_request("/health", 200)
                assert health == {"status": "ok", "message": "I'm alive", "version": version}, health
                # Explicit routes beat IIIF, even when their suffix is valid IIIF.
                assert request("/hello/id/full/max/0/default.jpg")[0] == 200
                assert request("/HELLO/id/full/max/0/default.jpg")[0] == 200
                assert request("/HEALTH")[0] == 200  # Drogon route matching is case-insensitive.

                for path in ("/id/full/max/0/default.jpg", "/myproj/subpart/id/full/max/0/default.jpg",
                             "/hello-world/full/max/0/default.jpg", "/a%2Fb/full/max/0/default.jpg",
                             "/a%252Fb/full/max/0/default.jpg", "/a+b/full/max/0/default.jpg",
                             "/id/full/max/0/default.jpg?size=bad"):
                    assert request(path)[0] == 200
                assert request('/id/%66ull/max/%210/default%2Ejpg')[0] == 200

                for name in ('source.jp2', 'source.j2k', 'offset.jp2', 'metadata-box.jp2', 'source.jph', 'source.jhc', 'offset.jph'):
                    _, info = json_request('/museum/'+name+'/info.json', 200)
                    assert (info['width'], info['height']) == (37, 23)
                    assert info['id'].endswith('/museum/'+name)
                    assert info['profile']=='level2'
                    assert info.get('extraQualities', []) == (['gray', 'bitonal'] if name in ('source.j2k','source.jhc') else ['color','gray','bitonal'])
                    status, _, data = request('/'+name+'/info.json', method='HEAD')
                    assert status == 200 and not data
                    assert request('/'+name+'/full/max/0/default.jpg')[0] == 200
                    path = '/'+name+'/3,4,19,8/max/0/default.'
                    status, _, body = request(path+'png')
                    assert status == 200
                    w,h,pixels,icc,_ = decode_png(body)
                    channels = 1 if name in ('source.j2k','source.jhc') else 3
                    expected = bytes(((x+3)*7+(y+4)*11+c*31)%256
                                     for y in range(8) for x in range(19) for c in range(channels))
                    assert (w,h,pixels,icc) == (19,8,expected,None)
                    status,_,jpg = request(path+'jpg')
                    assert status == 200
                    jp = Path(directory,'j2k-response.jpg'); jp.write_bytes(jpg)
                    rp = Path(directory,'j2k-reference.rgb')
                    rp.write_bytes(pixels if channels == 3 else bytes(v for v in pixels for _ in range(3)))
                    subprocess.run([str(verifier),'--rgb',str(jp),str(rp),'19','8'],check=True,timeout=5)
                    status,headers,empty = request(path+'png','HEAD')
                    assert status == 200 and not empty and int(headers['content-length']) == len(body)
                    status,_,reduced = request('/'+name+'/1,1,35,21/7,4/0/default.png')
                    assert status == 200 and decode_png(reduced)[:2] == (7,4)
                    assert request('/'+name+'/100,0,1,1/max/0/default.jpg')[0] == 400
                assert request('/broken.jp2/info.json')[0] == 500
                assert request('/corrupt-tile.jp2/0,0,5,5/max/0/default.png')[0] == 200
                assert request('/corrupt-tile.jp2/full/max/0/default.png')[0] == 500
                for name in ('sixteen.jp2','rgb-unknown.j2k'):
                    assert request('/'+name+'/info.json')[0] == 501
                    assert request('/'+name+'/full/max/0/default.png')[0] == 501

                # Public identity is independent of filesystem selection, Host and proxy headers.
                for path, service_path, dimensions in (
                    ("/id/info.json", "/id", (320, 200)),
                    ("/myproj/subpart/id/info.json", "/myproj/subpart/id", (320, 200)),
                    ("/health/id/info.json", "/health/id", (320, 200)),
                    ("/a%2fb/info.json", "/a%2Fb", (320, 200)),
                    ("/a%252Fb/info.json", "/a%252Fb", (320, 200)),
                    ("/a+b/info.json", "/a%2Bb", (320, 200)),
                    ("/caf%C3%A9.tif/info.json", "/caf%C3%A9.tif", (320, 200)),
                    ("/source%20with%20spaces.tif/info.json?token=secret", "/source%20with%20spaces.tif", (320, 200)),
                    ("/large.tif/info.json", "/large.tif", (1000, 800)),
                    ("/inside-link/info.json", "/inside-link", (320, 200)),
                ):
                    headers, document = json_request(path, 200)
                    assert document == {
                        "@context": "http://iiif.io/api/image/3/context.json",
                        "id": public_base + service_path,
                        "type": "ImageService3", "profile": "level2", "protocol": "http://iiif.io/api/image",
                        "width": dimensions[0], "height": dimensions[1], "extraFormats": ["webp"],
                        "maxWidth": 8192, "maxHeight": 8192, "maxArea": 8388608,
                        **({"extraFeatures": ["canonicalLinkHeader", "mirroring", "profileLinkHeader", "rotationArbitrary"] + (["sizeUpscaling"] if upscaling else []), "extraQualities": ["color", "gray", "bitonal"]}
                           if service_path != "/rotated.tif" else {}),
                    }, (path, document)
                    assert headers.get("cache-control") == "no-store"
                    status, head_headers, body = request(path, "HEAD")
                    assert status == 200 and not body
                    for field in ("content-type", "content-length", "cache-control"):
                        assert head_headers.get(field) == headers.get(field), (field, head_headers, headers)
                status, headers, body = request("/myproj/id/info.json", headers={
                    "Host": "attacker.test", "Forwarded": "host=attacker.test;proto=https",
                    "X-Forwarded-Host": "attacker.test", "X-Forwarded-Proto": "https"})
                assert status == 200 and json.loads(body)["id"] == public_base + "/myproj/id"

                for suffix, crop in (
                    ("full/max/0/default.jpg", (0, 0, 320, 200)),
                    ("50,30,100,80/max/0/default.jpg", (50, 30, 100, 80)),
                    ("square/max/0/default.jpg", (60, 0, 200, 200)),
                    ("pct:25,25,50,50/max/0/default.jpg", (80, 50, 160, 100)),
                    ("310,190,50,50/max/0/default.jpg", (310, 190, 10, 10)),
                    ("full/320,200/360/color.jpg", (0, 0, 320, 200)),
                    ("full/160,/0/default.jpg", (0, 0, 320, 200, 160, 100)),
                    ("full/pct:25/0/default.jpg", (0, 0, 320, 200, 80, 50)),
                    ("full/!90,70/0/default.jpg", (0, 0, 320, 200, 90, 56)),
                    ("full/,50/0/default.jpg", (0, 0, 320, 200, 80, 50)),
                    ("50,30,100,80/37,29/0/default.jpg", (50, 30, 100, 80, 37, 29)),
                    ("full/1,1/0/default.jpg", (0, 0, 320, 200, 1, 1)),
                    ("full/320,1/0/default.jpg", (0, 0, 320, 200, 320, 1)),
                ):
                    path = "/project/tiled.tif/" + suffix
                    status, headers, body = request(path)
                    assert status == 200 and headers.get("content-type", "").startswith("image/jpeg"), (status, headers, body)
                    assert body[:2] == b"\xff\xd8" and body[-2:] == b"\xff\xd9"
                    assert headers.get("cache-control") == "no-store"
                    assert int(headers["content-length"]) == len(body)
                    output = Path(directory, "response.jpg")
                    output.write_bytes(body)
                    subprocess.run([str(verifier), str(output), *map(str, crop)], check=True, timeout=5)
                    head_status, head_headers, head_body = request(path, "HEAD")
                    assert head_status == 200 and not head_body
                    for field in ("content-type", "content-length", "cache-control"):
                        assert head_headers.get(field) == headers.get(field)
                    # PNG must preserve the processed samples exactly. Reference
                    # each separable gradient axis by counting common-grid cells.
                    status, png_headers, png_body = request(path.replace('.jpg', '.png'))
                    assert status == 200 and png_headers['content-type'].startswith('image/png')
                    pw, ph, rgb, icc, chunks = decode_png(png_body)
                    rx, ry, rw, rh = crop[:4]
                    ow, oh = crop[4:] if len(crop) == 6 else (rw, rh)
                    assert (pw, ph) == (ow, oh) and icc is None
                    reds = [(2*sum(255*(rx+(x*rw+k)//ow)//319 for k in range(rw))+rw)//(2*rw) for x in range(ow)]
                    greens = [(2*sum(255*(ry+(y*rh+k)//oh)//199 for k in range(rh))+rh)//(2*rh) for y in range(oh)]
                    assert rgb == bytes(v for y in range(oh) for x in range(ow) for v in (reds[x],greens[y],128))
                    assert not {b'sRGB', b'gAMA', b'cHRM'}.intersection(chunks)
                    ps, hh, hb = request(path.replace('.jpg', '.png'), 'HEAD')
                    assert ps == 200 and not hb and hh['content-length'] == str(len(png_body))
                    assert png_headers['cache-control'] == 'no-store'
                # The base contains a corrupt tile. Success plus identical JPEG
                # bytes proves that the reduced SubIFD and inherited ICC are used.
                status, headers, body = request("/pyramid.tif/full/26,20/0/default.jpg")
                assert status == 200
                assert body == request("/pyramid-level.tif/full/max/0/default.jpg")[2]
                assert request("/pyramid.tif/full/max/0/default.jpg")[0] == 500
                assert request("/pyramid.tif/3,5,65,49/15,12/0/default.jpg")[0] == 200
                assert request("/pyramid-stripped.tif/full/26,20/0/default.jpg")[0] == 200
                status, head_headers, head_body = request("/pyramid.tif/full/26,20/0/default.jpg", "HEAD")
                assert status == 200 and not head_body
                assert head_headers["content-length"] == str(len(body))
                _, error = json_request("/invalid-profile.tif/full/max/0/default.png", 500)
                assert error['error'] == 'encoding_failed'
                assert request("/invalid-profile.tif/full/max/0/default.png", 'HEAD')[0] == 500
                status, headers, body = request("/pyramid.tif/full/26,20/0/default.png")
                assert status == 200 and body == request("/pyramid-level.tif/full/max/0/default.png")[2]
                pw, ph, rgb, icc, _ = decode_png(body)
                assert (pw, ph) == (26,20) and icc is not None and len(icc) == 132
                assert rgb == bytes(v for y in range(20) for x in range(26) for v in (x*3+40,y*4+60,100))
                assert request("/pyramid.tif/full/max/0/default.png")[0] == 500
                status, _, body = request("/pyramid.tif/3,5,65,49/15,12/0/default.png")
                assert status == 200 and decode_png(body)[:2] == (15,12)
                for name, region, output, level, associated in (
                    ('alpha.tif',(0,0,35,19),(35,19),(35,19),False),
                    ('alpha-associated.tif',(0,0,35,19),(35,19),(35,19),True),
                    ('alpha.tif',(3,1,29,17),(14,8),(35,19),False),
                    ('alpha-pyramid.tif',(3,1,29,17),(14,8),(18,10),False),
                ):
                    suffix = ','.join(map(str,region))+'/'+','.join(map(str,output))+'/0/default.'
                    path = '/'+name+'/'+suffix
                    status, headers, body = request(path+'png')
                    assert status == 200 and headers['content-type'].startswith('image/png')
                    w,h,rgba,icc,_ = decode_png(body)
                    assert (w,h) == output and icc is not None and len(rgba)==w*h*4
                    if output == region[2:]:
                        expected = bytes(v for y in range(h) for x in range(w) for v in alpha_pixel(x,y,associated))
                        assert rgba == expected
                    else:
                        expected = alpha_reference(region,output,level,associated)
                        assert all(abs(a-b)<=1 for a,b in zip(rgba,expected))
                    status, jpeg_headers, jpeg = request(path+'jpg')
                    assert status == 200 and jpeg_headers['content-type'].startswith('image/jpeg')
                    jpg_file = Path(directory,'alpha.jpg'); jpg_file.write_bytes(jpeg)
                    rgb_file = Path(directory,'alpha.rgb')
                    rgb_file.write_bytes(bytes((rgba[i+c]*rgba[i+3]+255*(255-rgba[i+3])+127)//255
                                               for i in range(0,len(rgba),4) for c in range(3)))
                    subprocess.run([str(verifier),'--rgb',str(jpg_file),str(rgb_file),str(w),str(h)], check=True,timeout=5)
                    for extension, content in (('png',body),('jpg',jpeg)):
                        hs,hh,hb = request(path+extension,'HEAD')
                        assert hs == 200 and not hb and hh['content-length']==str(len(content))
                assert request('/alpha-pyramid.tif/full/max/0/default.png')[0] == 500
                assert request('/alpha-unspecified.tif/full/max/0/default.png')[0] == 501
                assert request('/alpha-invalid.tif/full/max/0/default.png')[0] == 500
                # Strip RGB follows exactly the same crop/resize path as tiled RGB.
                for suffix in ('full/max/0/default.png','50,30,100,80/37,29/0/default.png',
                               '310,190,50,50/max/0/default.png'):
                    assert request('/sample.tif/'+suffix)[2] == request('/tiled.tif/'+suffix)[2]
                assert request('/sample.tif/full/^160,/0/default.jpg')[0] == 200
                assert request('/large.tif/full/900,/0/default.jpg')[0] == 200
                gray_bodies = []
                for name in ('gray.tif','gray-white.tif','gray-tiled.tif'):
                    path = '/'+name+'/3,4,19,8/max/0/default.'
                    status,headers,body = request(path+'png')
                    assert status == 200
                    w,h,pixels,icc,_ = decode_png(body)
                    expected = bytes(((x+3)*7+(y+4)*11)%256 for y in range(8) for x in range(19))
                    assert (w,h,pixels)==(19,8,expected) and icc[16:20]==b'GRAY'
                    gray_bodies.append(body)
                    assert request(path.replace('default.','gray.')+'png')[2] == body
                    assert request(path.replace('default.','color.')+'png')[2] == body
                    status,jh,jpeg = request(path+'jpg')
                    assert status == 200
                    jpg_file=Path(directory,'gray.jpg'); jpg_file.write_bytes(jpeg)
                    rgb_file=Path(directory,'gray.rgb'); rgb_file.write_bytes(bytes(v for v in pixels for _ in range(3)))
                    subprocess.run([str(verifier),'--gray',str(jpg_file),str(rgb_file),'19','8'],check=True,timeout=5)
                    hs,hh,hb=request(path+'png','HEAD')
                    assert hs==200 and not hb and hh['content-length']==str(len(body))
                    _,document=json_request('/'+name+'/info.json',200)
                    assert document['extraQualities']==['gray', 'bitonal'] and (document['width'],document['height'])==(37,23)
                assert gray_bodies[0]==gray_bodies[1]==gray_bodies[2]
                from fractions import Fraction as F
                from math import floor,ceil
                status,_,body=request('/gray-pyramid.tif/3,1,31,21/15,10/0/gray.png')
                assert status==200
                w,h,pixels,icc,_=decode_png(body)
                assert (w,h)==(15,10) and icc[16:20]==b'GRAY'
                for y in range(h):
                    top,dy=F(12,23)+y*F(21*12,23*h),F(21*12,23*h)
                    for x in range(w):
                        left,dx=F(3*19,37)+x*F(31*19,37*w),F(31*19,37*w)
                        total=sum((min(top+dy,sy+1)-max(top,sy))*(min(left+dx,sx+1)-max(left,sx))*((sx*7+sy*11)%256)
                                  for sy in range(floor(top),ceil(top+dy)) for sx in range(floor(left),ceil(left+dx)))
                        assert abs(pixels[y*w+x]-floor(total/(dx*dy)+F(1,2)))<=1
                assert request('/gray-pyramid.tif/full/max/0/default.png')[0]==500
                status,_,body=request('/strip-rgba.tif/3,4,19,8/max/0/default.png')
                assert status==200
                w,h,pixels,_,_=decode_png(body)
                expected=bytes(v for y in range(4,12) for x in range(3,22)
                               for v in (*[((x*7+y*11)%256+c*31)%256 for c in range(3)],((x+y)%3)*127))
                assert (w,h,pixels)==(19,8,expected)
                for suffix in ("full/max/0/default.tif",):
                    _, body = json_request("/tiled.tif/" + suffix, 400)
                    assert body["error"] == "unsupported_format", body
                _, body = json_request("/broken-tile.tif/full/max/0/default.jpg", 500)
                assert body["error"] == "invalid_image_source"
                # A bad tile outside the requested crop is never decoded.
                assert request("/broken-tile.tif/100,100,20,20/max/0/default.jpg")[0] == 200
                assert request("/tiled.tif/full/max/0/default.jpg")[0] == 200
                if configured:
                    assert request("/jpeg-alias/full/max/0/default.jpg")[0] == 200

                invalid = {
                    "/": "path", "/health/": "path",
                    "/id/no-region/max/0/default.jpg": "region",
                    "/id/full/0,/0/default.jpg": "size",
                    "/id/full/max/361/default.jpg": "rotation",
                    "/id/full/max/0/default.jpeg": "quality_format",
                    "/id//max/0/default.jpg": "path",
                    "/id%/full/max/0/default.jpg": "path",
                    "/id%2/full/max/0/default.jpg": "path",
                    "/id%GG/full/max/0/default.jpg": "path",
                    "/id%00/full/max/0/default.jpg": "path",
                    "/project/%2e%2e/id/info.json": "path",
                    # One decode leaves a percent sequence in the parameter, which is invalid.
                    "/id/%2566ull/max/0/default.jpg": "region",
                }
                for path, component in invalid.items():
                    _, body = json_request(path, 400)
                    assert body["error"] == "invalid_iiif_syntax", (path, body)
                    assert body["component"] == component and body["message"], (path, body)

                for path in ("/health", "/hello", "/hello/id/full/max/0/default.jpg",
                             "/id/full/max/0/default.jpg", "/id/info.json", "/missing"):
                    for method in ("POST", "PUT", "DELETE", "PATCH"):
                        headers, body = json_request(path, 405, method)
                        assert headers.get("allow") == "GET, HEAD, OPTIONS", headers
                        assert body["error"] == "method_not_allowed", body
                for path, expected_status in (("/hello", 200), ("/health", 200),
                                               ("/id/full/max/0/default.jpg", 200),
                                               ("/id/info.json", 200), ("/id/no-region/max/0/default.jpg", 400)):
                    status, _, body = request(path, "HEAD")
                    assert status == expected_status and not body, (path, status, body)
                assert request("/sample.tif/full/^640,/0/default.jpg")[0] == (200 if upscaling else 501)
                # Both default mapping and Lua-selected paths feed actual header dimensions.
                for path, status, code in (
                    ("/sample.tif/320,0,1,1/max/0/default.jpg", 400, "invalid_iiif_geometry"),
                    ("/sample.tif/full/321,/0/default.jpg", 400, "invalid_iiif_geometry"),

                    ("/sample.tif/pct:100,0,10,10/max/0/default.jpg", 400, "invalid_iiif_geometry"),
                    ("/rotated.tif/210,0,1,1/max/0/default.jpg", 501, "source_not_deliverable"),
                    ("/rotated.tif/0,210,1,1/max/0/default.jpg", 501, "source_not_deliverable"),
                    ("/sample.tif/full/900,/0/default.jpg", 400, "invalid_iiif_geometry"),
                    ("/broken.tif/info.json", 500, "invalid_image_source"),
                    ("/other.png/info.json", 500, "invalid_image_source"),
                    ("/..%2Fpolicy%20with%20spaces.lua/info.json", 403, "source_not_allowed"),
                    ("/outside-link%2Fpolicy%20with%20spaces.lua/info.json", 403 if configured else 404,
                     "source_not_allowed" if configured else "image_not_found"),
                ):
                    headers, body = json_request(path, status)
                    assert body["error"] == code, (path, body)
                    assert headers.get("cache-control") == "no-store"
                    assert request(path, "HEAD")[0] == status
                if configured:
                    for identifier, status, code in (
                        ("denied", 403, "access_denied"),
                        ("script-error", 500, "preflight_failed"),
                        ("invalid-result", 500, "preflight_failed"),
                        ("loop", 500, "preflight_failed"),
                        ("memory", 500, "preflight_failed"),
                        ("missing-source", 404, "image_not_found"),
                        ("directory-source", 404, "image_not_found"),
                        ("no-source", 404, "image_not_found"),
                    ):
                        for suffix in ("info.json", "full/max/0/default.jpg"):
                            path = f"/{identifier}/{suffix}"
                            headers, body = json_request(path, status)
                            assert body["error"] == code, body
                            assert headers.get("cache-control") == "no-store", headers
                            assert "secret-token" not in str(body) and "/private/" not in str(body), body
                            head_status, _, head_body = request(path, "HEAD")
                            assert head_status == status and not head_body
                    assert request("/use-large/full/900,/0/default.jpg")[0] == 200
                    # A trusted script may deliberately select outside the repository.
                    outside = Path(directory, "outside.tif")
                    shutil.copyfile(fixtures / "sample.tif", outside)
                    assert request("/absolute/info.json", headers={"Authorization": str(outside)})[0] == 200
                    assert request("/credentials/info.json")[0] == 403
                    assert request("/credentials/info.json", headers={
                        "Authorization": "Bearer test-token", "Cookie": "session=abc"})[0] == 200
                    assert request("/credentials/info.json")[0] == 403
                    _, encoded = json_request("/project%2fpart/encoded%2fid/info.json", 200)
                    assert encoded["id"] == public_base + "/project%2Fpart/encoded%2Fid"
                    _, alternate = json_request("/project/use-large/info.json", 200)
                    assert (alternate["width"], alternate["height"]) == (1000, 800)
                    assert alternate["id"] == public_base + "/project/use-large"
                    # Syntax validation and explicit routes precede preflight.
                    assert request("/denied/bad/max/0/default.jpg")[0] == 400
                    assert request("/hello/denied/info.json")[0] == 200
                    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
                        pending = [executor.submit(request, "/loop/info.json") for _ in range(16)]
                        assert request("/health")[0] == 200
                        assert all(result.result()[0] in (500, 503) for result in pending)
                    assert request("/id/info.json")[0] == 200  # worker recovers after failures
                    log.flush()
                    log.seek(0)
                    recorded = log.read()
                    assert "secret-token" not in recorded
                    assert "[redacted] /private/source.tif" in recorded
                    assert "script_execution; category 2" in recorded
                process.send_signal(signal.SIGTERM)
                assert process.wait(timeout=5) == 0
                print("HTTP smoke checks passed (including SIGTERM shutdown)")
            except BaseException:
                log.seek(0)
                print(log.read(), file=sys.stderr)
                raise
            finally:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


def startup_errors(executable: Path) -> None:
    """Invalid configured policies fail before serving; help/version bypass I/O."""
    env = {key: value for key, value in os.environ.items() if not key.startswith("SIPI_")}
    with tempfile.TemporaryDirectory(prefix="sipi-startup-") as directory:
        for name, code in {
            "syntax.lua": "function preflight(",
            "missing-function.lua": "local value = 42",
            "init-error.lua": "error('initialization failed')",
        }.items():
            Path(directory, name).write_text(code)
        for name in ("absent.lua", "syntax.lua", "missing-function.lua", "init-error.lua"):
            result = subprocess.run([str(executable), "--preflight-script", name, "--log-level", "off"],
                                    cwd=directory, env=env, capture_output=True, text=True, timeout=5)
            assert result.returncode == 2 and "Preflight startup error:" in result.stderr, result
            assert "listening" not in result.stdout, result
        result = subprocess.run([str(executable)], cwd=directory,
                                env={**env, "SIPI_PREFLIGHT_SCRIPT": "absent.lua"},
                                capture_output=True, text=True, timeout=5)
        assert result.returncode == 2 and "Preflight startup error:" in result.stderr, result
        for root in ("absent-root", "syntax.lua"):
            result = subprocess.run([str(executable), "--iiif-root", root], cwd=directory,
                                    env=env, capture_output=True, text=True, timeout=5)
            assert result.returncode == 2 and "Image repository startup error:" in result.stderr, result
        # Exercise main's actual environment snapshot, not only the loader API.
        secret = "do-not-disclose-this-value"
        for variables in ({"SIPI_S3_ACCESS_KEY": "access"},
                          {"SIPI_S3_SECRET_KEY": secret},
                          {"SIPI_S3_SESSION_TOKEN": secret}):
            result = subprocess.run([str(executable)], cwd=directory, env={**env, **variables},
                                    capture_output=True, text=True, timeout=5)
            assert result.returncode == 2 and "Configuration error:" in result.stderr, result
            assert secret not in result.stdout + result.stderr, result
        result = subprocess.run([str(executable), "--s3-access-key=cli-access", "--iiif-root=absent-root"],
                                cwd=directory, env={**env, "SIPI_S3_ACCESS_KEY": "", "SIPI_S3_SECRET_KEY": secret},
                                capture_output=True, text=True, timeout=5)
        assert result.returncode == 2 and "Image repository startup error:" in result.stderr, result
        assert secret not in result.stdout + result.stderr, result
        for action in ("--help", "--version"):
            result = subprocess.run([str(executable), action, "--iiif-root", "absent-root"],
                                    cwd=directory, env=env, capture_output=True, text=True, timeout=5)
            assert result.returncode == 0 and not result.stderr, result
            result = subprocess.run([str(executable), action, "--preflight-script", "absent.lua"],
                                    cwd=directory, env=env, capture_output=True, text=True, timeout=5)
            assert result.returncode == 0 and not result.stderr, result
    print("Preflight startup error checks passed")


if __name__ == "__main__":
    executable = Path(sys.argv[1]).resolve()
    startup_errors(executable)
    fixtures = Path(sys.argv[2]).resolve()
    verifier = Path(sys.argv[3]).resolve()
    run(executable, fixtures, verifier)
    run(executable, fixtures, verifier, configured=True)
    run(executable, fixtures, verifier, upscaling=True)
