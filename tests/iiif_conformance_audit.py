"""Diagnostic IIIF 3 audit against a private server and generated PNG sources.

Run with the sipi-2 executable as the only argument. Prints JSON evidence and exits
1 when gaps remain, 0 otherwise. This is a focused audit, not IIIF certification or
an exhaustive validator; it deliberately stays outside the green CTest suite.
No external server or repository images are modified.
"""
from pathlib import Path
import http.client
import json
import struct
import sys
import tempfile
from cache_http import server
from png_metadata import fixture
from png_check import decode_png


def main():
    executable = Path(sys.argv[1]).resolve()
    checks = []

    def record(name, passed, evidence):
        checks.append(dict(check=name, passed=bool(passed), evidence=evidence))

    with tempfile.TemporaryDirectory(prefix="sipi-iiif-audit-") as temporary:
        root = Path(temporary)
        (root/"images").mkdir()
        for name, options in {
            "color.png": dict(width=320, height=200),
            "gray.png": dict(color=0, width=320, height=200),
            "wide.png": dict(color=0, width=70000, height=1),
            "sixteen.png": dict(bits=16),
        }.items():
            (root/"images"/name).write_bytes(fixture(**options))
        # Minimal valid gray TIFF with orientation 6: metadata supported, pixels rejected.
        tags=[(256,4,2),(257,4,3),(258,3,8),(259,3,1),(262,3,1),
              (273,4,146),(274,3,6),(277,3,1),(278,4,3),(279,4,6),(284,3,1)]
        tiff=b"II"+struct.pack("<HIH",42,8,len(tags))
        tiff+=b"".join(struct.pack("<HHII",tag,kind,1,value) for tag,kind,value in tags)
        tiff+=struct.pack("<I",0)+bytes([0,50,100,150,200,255])
        (root/"images"/"oriented.tif").write_bytes(tiff)
        (root/"policy.lua").write_text("function preflight(r) return {decision='allow'} end")

        with server(executable, root, cache=None) as (port, _):
            def request(path, method="GET", headers=None):
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=15)
                try:
                    connection.request(method, path, headers=headers or {})
                    response = connection.getresponse()
                    return response.status, {k.lower():v for k,v in response.getheaders()}, response.read()
                finally:
                    connection.close()

            status, headers, body = request("/color.png/info.json")
            info = json.loads(body)
            record("info.required_profile", status==200 and info.get("profile") in ("level0","level1","level2"), info)
            record("info.color_quality", "color" in info.get("extraQualities", []), info.get("extraQualities"))
            record("info.png_discovery", info.get("profile")=="level2" or "png" in info.get("extraFormats", []),
                   dict(profile=info.get("profile"), extraFormats=info.get("extraFormats")))
            status, redirect, _ = request("/project/color.png")
            record("http.base_redirect", status==303 and redirect.get("location","").endswith("/project/color.png/info.json"),
                   dict(status=status, location=redirect.get("location")))
            status, ld, _ = request("/color.png/info.json", headers={"Accept":"application/ld+json"})
            record("http.jsonld", status==200 and ld.get("content-type","").startswith("application/ld+json"),
                   dict(status=status, contentType=ld.get("content-type")))
            for path in ("/color.png/info.json", "/color.png/full/max/0/default.png", "/missing.png/info.json"):
                status, cors, _ = request(path, headers={"Origin":"https://viewer.example"})
                record("http.cors:"+path, cors.get("access-control-allow-origin") in ("*","https://viewer.example"),
                       dict(status=status, allowOrigin=cors.get("access-control-allow-origin")))
            status, cors, _ = request("/color.png/info.json", "OPTIONS",
                {"Origin":"https://viewer.example", "Access-Control-Request-Method":"GET",
                 "Access-Control-Request-Headers":"Authorization"})
            record("http.cors_preflight", status in (200,204) and
                   "authorization" in cors.get("access-control-allow-headers","").lower(),
                   dict(status=status, headers=cors))

            for region, size, dimensions in (
                ("full","max",(320,200)), ("10,20,100,80","max",(100,80)),
                ("pct:10,20,50,50","max",(160,100)), ("square","max",(200,200)),
                ("full","160,",(160,100)), ("full",",100",(160,100)),
                ("full","pct:50",(160,100)), ("full","!100,100",(100,63)),
                ("full","100,100",(100,100)), ("300,190,100,100","max",(20,10)),
            ):
                status, _, data = request(f"/color.png/{region}/{size}/0/default.png")
                actual = decode_png(data)[:2] if status==200 else None
                record(f"geometry:{region}/{size}", status==200 and actual==dimensions,
                       dict(status=status, dimensions=actual))
            for angle, dimensions in (("90",(200,320)), ("180",(320,200)), ("270",(200,320)), ("!0",(320,200))):
                status, _, data = request(f"/color.png/full/max/{angle}/default.png")
                actual = decode_png(data)[:2] if status==200 else None
                record("rotation:"+angle, status==200 and actual==dimensions, dict(status=status, dimensions=actual))
            for quality in ("default","color","gray","bitonal"):
                for extension in ("jpg","png"):
                    path=f"/color.png/full/160,100/0/{quality}.{extension}"
                    status, output_headers, data=request(path)
                    record("delivery:"+quality+"."+extension, status==200 and
                           output_headers.get("content-type","").startswith("image/"+("jpeg" if extension=="jpg" else "png")),
                           dict(status=status, contentType=output_headers.get("content-type")))
            path="/color.png/full/max/0/default.png"
            status, image_headers, image=request(path)
            head_status, head_headers, head_body=request(path,"HEAD")
            record("http.head", head_status==status==200 and not head_body and
                   int(head_headers.get("content-length","-1"))==len(image), dict(status=head_status, bytes=len(head_body)))
            for relation in ("profile","canonical"):
                record("optional.link:"+relation, f'rel="{relation}"' in image_headers.get("link",""),
                       image_headers.get("link"))
            status, _, _=request("/color.png/full/max/0/default.tif")
            record("error.unsupported_format", status==400, dict(status=status, recommended=400))
            status, _, _=request("/color.png/full/^pct:120/0/default.jpg")
            record("error.unsupported_upscaling", status==501, dict(status=status))
            status, _, _=request("/color.png/400,0,1,1/max/0/default.jpg")
            record("error.outside_region", status==400, dict(status=status))
            status, _, data=request("/project/sub/color.png/info.json")
            record("identity.prefix", status==200 and json.loads(data)["id"].endswith("/project/sub/color.png"),
                   dict(status=status))
            status, _, data=request("/sixteen.png/info.json")
            record("source.unsupported_png_precision", status==501, dict(status=status))
            status, _, data=request("/oriented.tif/info.json")
            metadata=json.loads(data)
            pixel_status, _, _=request("/oriented.tif/full/max/0/default.jpg")
            record("source.metadata_only", status!=200 or pixel_status==200,
                   dict(infoStatus=status, imageStatus=pixel_status, document=metadata))
            status, _, data=request("/wide.png/info.json")
            wide=json.loads(data)
            image_status, _, image_body=request("/wide.png/full/max/0/default.jpg")
            record("limits.wide_jpeg", image_status==200, dict(infoStatus=status, width=wide.get("width"),
                   maxWidth=wide.get("maxWidth"), imageStatus=image_status,
                   error=json.loads(image_body) if image_status!=200 else None))
    report=dict(passed=sum(c["passed"] for c in checks), total=len(checks), checks=checks)
    print(json.dumps(report, indent=2))
    return 0 if report["passed"]==report["total"] else 1


if __name__=="__main__":
    sys.exit(main())
