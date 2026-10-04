"""Exercise the real HTTP listener, URL escaping, routing, and graceful shutdown.

Uses only Python's standard library. Starts the executable in an isolated
working directory on a free loopback port and always reaps the child process.
"""

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
        for name in ("sample.tif", "rotated.tif", "large.tif", "tiled.tif", "broken-tile.tif"):
            shutil.copyfile(fixtures / name, root / name)
        for name in ("id", "hello-world", "a/b", "a%2Fb", "a+b", "café.tif", "source with spaces.tif"):
            target = root / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(fixtures / "sample.tif", target)
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
    if id == 'tiled.tif' or id == 'broken-tile.tif' then return {decision='allow', source=id} end
    if id == 'denied' then return {decision='deny'} end
    if id == 'script-error' then error('secret-token /private/source.tif') end
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
            arguments += ["--config", "config/server.toml"]
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
                    assert "application/json" in headers.get("content-type", ""), headers
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
                             "/id/%66ull/max/%210/default%2Ejpg", "/id/full/max/0/default.jpg?size=bad"):
                    _, body = json_request(path, 501)
                    assert body["error"] == "not_implemented" and body["message"], body

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
                    ("/rotated.tif/info.json", "/rotated.tif", (200, 320)),
                    ("/large.tif/info.json", "/large.tif", (1000, 800)),
                    ("/inside-link/info.json", "/inside-link", (320, 200)),
                ):
                    headers, document = json_request(path, 200)
                    assert document == {
                        "@context": "http://iiif.io/api/image/3/context.json",
                        "id": public_base + service_path,
                        "type": "ImageService3", "protocol": "http://iiif.io/api/image",
                        "width": dimensions[0], "height": dimensions[1],
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
                for suffix in ("full/160,/0/default.jpg", "full/max/90/default.jpg",
                               "full/max/!0/default.jpg", "full/max/0/gray.jpg",
                               "full/max/0/bitonal.jpg", "full/max/0/default.png"):
                    _, body = json_request("/tiled.tif/" + suffix, 501)
                    assert body["error"] == "not_implemented", body
                _, body = json_request("/broken-tile.tif/full/max/0/default.jpg", 500)
                assert body["error"] == "invalid_image_source"
                # A bad tile outside the requested crop is never decoded.
                assert request("/broken-tile.tif/100,100,20,20/max/0/default.jpg")[0] == 200
                assert request("/tiled.tif/full/max/0/default.jpg")[0] == 200
                if configured:
                    assert request("/jpeg-alias/full/max/0/default.jpg")[0] == 200

                invalid = {
                    "/": "path", "/helloworld": "path", "/missing.html": "path", "/health/": "path",
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
                    for method in ("POST", "PUT", "DELETE", "PATCH", "OPTIONS"):
                        headers, body = json_request(path, 405, method)
                        assert headers.get("allow") == "GET, HEAD", headers
                        assert body["error"] == "method_not_allowed", body
                for path, expected_status in (("/hello", 200), ("/health", 200),
                                               ("/id/full/max/0/default.jpg", 501),
                                               ("/id/info.json", 200), ("/invalid", 400)):
                    status, _, body = request(path, "HEAD")
                    assert status == expected_status and body == "", (path, status, body)
                # Both default mapping and Lua-selected paths feed actual header dimensions.
                for path, status, code in (
                    ("/sample.tif/320,0,1,1/max/0/default.jpg", 400, "invalid_iiif_geometry"),
                    ("/sample.tif/full/321,/0/default.jpg", 400, "invalid_iiif_geometry"),
                    ("/sample.tif/full/^640,/0/default.jpg", 501, "not_implemented" if upscaling else "upscaling_disabled"),
                    ("/sample.tif/full/^160,/0/default.jpg", 501, "not_implemented"),
                    ("/sample.tif/310,190,50,50/max/0/default.jpg", 501, "not_implemented"),
                    ("/sample.tif/pct:100,0,10,10/max/0/default.jpg", 400, "invalid_iiif_geometry"),
                    ("/rotated.tif/210,0,1,1/max/0/default.jpg", 400, "invalid_iiif_geometry"),
                    ("/rotated.tif/0,210,1,1/max/0/default.jpg", 501, "not_implemented"),
                    ("/large.tif/full/900,/0/default.jpg", 501, "not_implemented"),
                    ("/sample.tif/full/900,/0/default.jpg", 400, "invalid_iiif_geometry"),
                    ("/broken.tif/info.json", 500, "invalid_image_source"),
                    ("/other.png/info.json", 501, "source_format_not_supported"),
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
                    assert request("/use-large/full/900,/0/default.jpg")[0] == 501
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
                    assert "secret-token" not in recorded and "/private/source.tif" not in recorded
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
