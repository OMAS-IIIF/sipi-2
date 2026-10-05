"""Metadata reuse across info/images, aliases, versions and process restart.

The imported server helper owns disposable listeners and captures debug logs.
No persistent result cache is enabled: header reuse is an independent feature.
"""
from cache_http import server, get, success
import json
import os
from pathlib import Path
import shutil
import sys
import tempfile


def main():
    executable, fixtures = (Path(arg).resolve() for arg in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="sipi-metadata-http-") as temporary:
        root = Path(temporary)
        images = root / "images"
        images.mkdir()
        for name in ("pyramid.tif", "source.jp2", "gray.tif", "pyramid-stripped.tif"):
            shutil.copyfile(fixtures/name, images/name)
        (images/"bad.tif").write_bytes(b"II*\0bad")
        (images/"unknown").write_bytes(b"unknown")
        (root/"policy.lua").write_text('''function preflight(request)
          if request.headers.authorization == 'denied' then return {decision='deny'} end
          if request.identifier == 'alias' then return {decision='allow',source='source.jp2'} end
          return {decision='allow'}
        end''')
        with server(executable, root, cache=None) as (port, log):
            for name in ("pyramid.tif","source.jp2","pyramid-stripped.tif"):
                info = get(port,f"/{name}/info.json")
                assert info[0]==200
                assert get(port,f"/project/{name}/info.json","HEAD")[0]==200
                path=f"/{name}/full/10,/0/default.png"
                first=success(get(port,path))
                assert success(get(port,path))==first
                # Planning must not modify cached directories.
                assert get(port,f"/{name}/info.json")[2]==info[2]
            assert json.loads(get(port,"/museum/alias/info.json")[2])["id"].endswith("/museum/alias")
            assert get(port,"/source.jp2/info.json",authorization="denied")[0]==403
            assert get(port,"/source.jp2/full/max/0/default.png",authorization="denied")[0]==403
            # Same pathname changes format and dimensions: always consult current version.
            shutil.copyfile(images/"gray.tif",images/"replacement")
            os.replace(images/"replacement",images/"source.jp2")
            info=json.loads(get(port,"/source.jp2/info.json")[2])
            expected=json.loads(get(port,"/gray.tif/info.json")[2])
            assert (info["width"],info["height"]) == (expected["width"],expected["height"])
            assert "gray" in info["extraQualities"]
            assert success(get(port,"/source.jp2/full/max/0/gray.png"))
            (images/"source.jp2").unlink()
            assert get(port,"/source.jp2/info.json")[0]==404
            for _ in range(2):
                assert get(port,"/bad.tif/info.json")[0]==500
                assert get(port,"/unknown/info.json")[0]==501
        assert log[0].count("IIIF metadata cache miss")==9, log[0]
        assert log[0].count("IIIF metadata cache hit")==14, log[0]
        assert "admission failed" not in log[0]
        with server(executable, root, cache=None) as (port, log):
            assert get(port,"/pyramid.tif/info.json")[0]==200
            assert get(port,"/pyramid.tif/info.json")[0]==200
        assert log[0].count("IIIF metadata cache miss")==1
        assert log[0].count("IIIF metadata cache hit")==1
        # Exercise the full env/config/CLI -> server -> worker constructor path.
        for args, variables in (
            (["--metadata-cache-max-entries=0"], {}),
            ([], {"SIPI_METADATA_CACHE_MAX_BYTES":"0"}),
        ):
            with server(executable, root, args, cache=None, variables=variables) as (port, log):
                assert get(port,"/pyramid.tif/info.json")[0]==200
                assert get(port,"/pyramid.tif/info.json")[0]==200
            assert log[0].count("IIIF metadata cache miss")==2
            assert "IIIF metadata cache hit" not in log[0]
        (root/"metadata.toml").write_text("metadata-cache-max-entries = 1\nmetadata-cache-max-bytes = 1048576\n")
        with server(executable, root, ["--config","metadata.toml"], cache=None) as (port, log):
            for name in ("pyramid.tif","gray.tif","pyramid.tif"):
                assert get(port,f"/{name}/info.json")[0]==200
        assert log[0].count("IIIF metadata cache miss")==3
        with server(executable, root, ["--metadata-cache-max-entries=256","--metadata-cache-max-bytes=67108864"],
                    cache=None, variables={"SIPI_METADATA_CACHE_MAX_ENTRIES":"0","SIPI_METADATA_CACHE_MAX_BYTES":"0"}) as (port, log):
            assert get(port,"/pyramid.tif/info.json")[0]==200
            assert get(port,"/pyramid.tif/info.json")[0]==200
        assert log[0].count("IIIF metadata cache hit")==1
    print("HTTP metadata reuse, authorization, replacement and restart passed")


if __name__ == "__main__":
    main()
