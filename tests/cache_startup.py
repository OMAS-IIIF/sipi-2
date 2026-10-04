"""Verify cache environment wiring, initialization, restart and failure fallback.

Only temporary directories and loopback listeners are used; child servers are
always stopped and reaped. Image lookup/fill is covered by cache_http.py.
"""
import http.client
import os
from pathlib import Path
import signal
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time


def run(executable, cwd, environment, arguments, inspect):
    """Wait for health, inspect the initialized cache, then stop the server."""
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    with tempfile.TemporaryFile(mode="w+") as output:
        process = subprocess.Popen([str(executable), "--port", str(port), *arguments],
                                   cwd=cwd, env=environment, stdout=output, stderr=output)
        try:
            deadline = time.monotonic() + 10
            while True:
                if process.poll() is not None:
                    raise AssertionError("Server exited before health response")
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=0.2)
                try:
                    connection.request("GET", "/health")
                    response = connection.getresponse()
                    assert response.status == 200
                    response.read()
                    break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise AssertionError("Server startup timeout")
                    time.sleep(0.03)
                finally:
                    connection.close()
            inspect()
        finally:
            process.send_signal(signal.SIGTERM)
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            output.seek(0)
            log = output.read()
            if process.returncode not in (0, -signal.SIGTERM):
                raise AssertionError(log)
        return log


def main():
    executable = Path(sys.argv[1]).resolve()
    environment = {k: v for k, v in os.environ.items() if not k.startswith("SIPI_")}
    with tempfile.TemporaryDirectory(prefix="sipi-cache-startup-") as temporary:
        root = Path(temporary)
        config = root / "settings.toml"
        config.write_text('cache-dir = "file-cache"\n')
        environment["SIPI_CACHE_DIR"] = "env-cache"
        environment["SIPI_CACHE_MAX_BYTES"] = "1024"
        environment["SIPI_CACHE_TARGET_PERCENT"] = "80"

        def inspect():
            assert not (root / "env-cache").exists()
            assert not (root / "file-cache").exists()
            with sqlite3.connect(root / "cli-cache" / "cache.sqlite3") as database:
                assert database.execute("PRAGMA user_version").fetchone() == (1,)
                assert database.execute("SELECT count(*) FROM entries").fetchone() == (0,)

        args = ["--config", str(config), "--cache-dir", "cli-cache"]
        for _ in range(2):
            log = run(executable, root, environment, args, inspect)
            assert "Persistent cache initialized" in log
        def inspect_environment():
            assert (root / "env-cache" / "cache.sqlite3").is_file()

        run(executable, root, environment, ["--config", str(config)], inspect_environment)
        (root / "blocked").write_text("not a directory")
        log = run(executable, root, environment, ["--cache-dir", "blocked"], lambda: None)
        assert "continuing without cache" in log
        # Informational actions must not create directories or load environment.
        environment["SIPI_CACHE_DIR"] = "help-only"
        subprocess.run([str(executable), "--help"], env=environment, cwd=root,
                       check=True, stdout=subprocess.DEVNULL)
        assert not (root / "help-only").exists()
    print("Cache startup tests passed")


if __name__ == "__main__":
    main()
