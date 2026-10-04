"""Exercise authorized HTTP cache fill/hit, source versions and failure fallback.

All fixtures/cache mutations are isolated in a disposable directory. SQLite
fault injection below deliberately violates normal operator ownership to prove
that an optional cache cannot turn good source requests into errors.
"""
import concurrent.futures
import contextlib
import http.client
import json
import os
from pathlib import Path
import shutil
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time


@contextlib.contextmanager
def server(executable, root, extra=(), cache="cache"):
    """Own one listener; retain its log for hit/miss assertions after shutdown."""
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    env = {k: v for k, v in os.environ.items() if not k.startswith("SIPI_")}
    log = []
    with tempfile.TemporaryFile(mode="w+") as output:
        args = [str(executable), "--port", str(port), "--iiif-root", "images",
                "--preflight-script", "policy.lua", "--log-level", "debug", *extra]
        if cache is not None:
            args += ["--cache-dir", cache]
        process = subprocess.Popen(args, cwd=root, env=env, stdout=output, stderr=output)
        try:
            for _ in range(150):
                if process.poll() is not None:
                    raise AssertionError("server exited before listening")
                try:
                    if get(port, "/health")[0] == 200:
                        break
                except OSError:
                    pass
                time.sleep(.03)
            else:
                raise AssertionError("listener timeout")
            yield port, log
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            output.seek(0)
            log.append(output.read())
            assert process.returncode == 0, log[0]


def get(port, path, method="GET", authorization="allowed"):
    with contextlib.closing(http.client.HTTPConnection("127.0.0.1", port, timeout=10)) as conn:
        conn.request(method, path, headers={"Authorization": authorization})
        result = conn.getresponse()
        return result.status, dict(result.getheaders()), result.read()


def rows(root, cache="cache"):
    with sqlite3.connect(root / cache / "cache.sqlite3") as db:
        return db.execute("SELECT digest,bytes,media,width,height FROM entries ORDER BY digest").fetchall()


def change_index(root, sql, args=(), cache="cache"):
    with sqlite3.connect(root / cache / "cache.sqlite3") as db:
        db.execute(sql, args)


def body_file(root, digest, cache="cache"):
    name = digest.decode("ascii")
    return root / cache / "objects" / name[:2] / (name + ".bin")


def success(result, mime="image/png"):
    status, headers, body = result
    headers = {k.lower(): v for k, v in headers.items()}
    assert status == 200, (status, body)
    assert headers["content-type"].startswith(mime), headers
    assert headers["cache-control"] == "no-store"
    assert int(headers["content-length"]) == len(body)
    assert body
    return body


def main():
    executable, fixtures = (Path(p).resolve() for p in sys.argv[1:])
    with tempfile.TemporaryDirectory(prefix="sipi-cache-http-") as temporary:
        root = Path(temporary)
        images = root / "images"
        images.mkdir()
        for name in ("tiled.tif", "gray.tif", "alpha.tif", "rotated.tif", "broken-tile.tif",
                     "invalid-profile.tif", "source.jp2", "sixteen.jp2", "corrupt-tile.jp2"):
            shutil.copyfile(fixtures / name, images / name)
        (root / "policy.lua").write_text('''function preflight(request)
          if request.headers.authorization == 'denied' then return {decision='deny'} end
          if request.headers.authorization == 'failure' then error('policy test') end
          if request.headers.authorization == 'alternate' then return {decision='allow',source='gray.tif'} end
          if request.identifier == 'alias' then return {decision='allow',source='tiled.tif'} end
          return {decision='allow'}
        end''')
        path = "/tiled.tif/full/80,/0/default.png"
        with server(executable, root) as (port, log):
            assert get(port, "/tiled.tif/info.json")[0] == 200
            assert rows(root) == []  # Information documents never enter the body cache.
            original = success(get(port, path))
            first = rows(root)
            assert len(first) == 1 and first[0][2:] == (b"image/png", 80, 50)
            for alias in (path, "/project/alias/full/pct:25/360/color.png",
                          "/tiled.tif/0,0,320,200/80,50/0/default.png"):
                assert success(get(port, alias)) == original
            status, headers, body = get(port, path, "HEAD")
            assert status == 200 and not body
            assert int({k.lower():v for k,v in headers.items()}["content-length"]) == len(original)
            assert rows(root) == first
            assert get(port, path, authorization="denied")[0] == 403
            assert get(port, path, "HEAD", "denied")[0] == 403
            assert get(port, "/tiled.tif/info.json", authorization="denied")[0] == 403
            assert get(port, path, authorization="failure")[0] == 500
            for bad in ("/tiled.tif/full/80,/90/default.png", "/tiled.tif/full/80,/!0/default.png",
                        "/tiled.tif/full/80,/0/gray.png", "/rotated.tif/full/max/0/default.png",
                        "/sixteen.jp2/full/max/0/default.png"):
                assert get(port, bad)[0] == 501, bad
            assert get(port, "/tiled.tif/999,0,1,1/max/0/default.png")[0] == 400
            assert rows(root) == first
            for bad in ("/broken-tile.tif/full/max/0/default.png", "/corrupt-tile.jp2/full/max/0/default.png",
                        "/invalid-profile.tif/full/max/0/default.png"):
                assert get(port, bad)[0] == 500, bad
            assert rows(root) == first  # Failed decodes/encodings never publish.
            jpeg = success(get(port, path.replace(".png", ".jpg")), "image/jpeg")
            assert success(get(port, path.replace(".png", ".jpg")), "image/jpeg") == jpeg
            assert len(rows(root)) == 2
            for name in ("source.jp2", "alpha.tif", "gray.tif"):
                request = f"/{name}/full/max/0/default.png"
                encoded = success(get(port, request))
                assert success(get(port, request)) == encoded
            gray = success(get(port, "/gray.tif/full/max/0/gray.png"))
            assert gray == success(get(port, "/gray.tif/full/max/0/color.png"))
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                assert all(success(result) == original for result in pool.map(lambda _:get(port,path),range(12)))
            snapshot = rows(root)
            # Missing/truncated body -> miss and complete regeneration.
            body_file(root, first[0][0]).unlink()
            assert success(get(port,path)) == original
            body_file(root, first[0][0]).write_bytes(b"partial")
            assert success(get(port,path)) == original
            assert rows(root) == snapshot
        assert log[0].count("IIIF result cache hit") >= 20, log[0]
        assert "lookup failed" not in log[0] and "publication failed" not in log[0], log[0]
        with server(executable, root) as (port, restart_log):
            assert success(get(port,path)) == original
            assert rows(root) == snapshot
            # Warm this exact URL before replacing its source, not only another size.
            changed_path = "/tiled.tif/full/max/0/default.png"
            old_full = success(get(port, changed_path))
            assert old_full != gray
            # The same URL can resolve to a different definitive source via preflight.
            assert success(get(port, changed_path, authorization="alternate")) == gray
            assert success(get(port, changed_path)) == old_full
            before_replacement = len(rows(root))
            # A new inode/version under the same ID cannot reuse old output.
            shutil.copyfile(images / "gray.tif", images / "replacement.tif")
            os.replace(images / "replacement.tif", images / "tiled.tif")
            assert success(get(port,changed_path)) == gray
            assert len(rows(root)) == before_replacement + 1
            assert get(port,path)[0] == 400  # Current geometry, not the old 80px hit.
            (images / "tiled.tif").unlink()
            assert get(port,changed_path)[0] == 404
        assert "IIIF result cache hit" in restart_log[0]
        shutil.copyfile(fixtures / "tiled.tif", images / "tiled.tif")

        # Tiny budget declines storage without changing the response.
        with server(executable, root, ["--cache-max-bytes=1"], cache="tiny") as (port, tiny_log):
            assert success(get(port,path)) == original
            assert success(get(port,path)) == original
            assert rows(root,"tiny") == []
        assert "admission declined" in tiny_log[0]
        # Isolate deliberate database faults from the normal cache.
        with server(executable, root, cache="fault") as (port, fault_log):
            change_index(root,"CREATE TRIGGER fail_insert BEFORE INSERT ON entries BEGIN SELECT RAISE(FAIL,'test'); END",cache="fault")
            assert success(get(port,path)) == original
            assert rows(root,"fault") == []
        assert "publication failed" in fault_log[0]
        change_index(root,"DROP TRIGGER fail_insert",cache="fault")
        with server(executable, root, cache="fault") as (port, fault_log):
            assert success(get(port,path)) == original
            change_index(root,"UPDATE entries SET width=1",cache="fault")
            assert success(get(port,path)) == original  # Inconsistent metadata cannot be a hit.
        assert "lookup failed" in fault_log[0]
        with server(executable, root, cache="fault") as (port, fault_log):
            change_index(root,"DROP TABLE entries",cache="fault")
            assert success(get(port,path)) == original
        assert "lookup failed" in fault_log[0]
        # No cache configuration retains the original behavior and output bytes.
        with server(executable, root, cache=None) as (port, disabled_log):
            assert success(get(port,path)) == original
        assert "IIIF result cache" not in disabled_log[0]
    print("HTTP cache authorization, reuse, restart, invalidation and fallback passed")


if __name__ == "__main__":
    main()
