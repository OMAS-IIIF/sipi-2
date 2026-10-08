"""Serve controlled S3-shaped replies to verify range and failure handling offline."""
import http.server
import subprocess
import sys
import threading
import re
from collections import Counter

reads = {}
reads_lock = threading.Lock()
object_size = 20*4096+17


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def reply(self, status, length, **headers):
        self.send_response(status)
        self.send_header("Content-Length", str(length))
        for key, value in headers.items():
            self.send_header(key, value)
        self.end_headers()

    def do_HEAD(self):
        key = self.path.rsplit("/", 1)[-1]
        if key.startswith("cache-"):
            self.reply(200, object_size, ETag='"original"')
        elif key == "missing":
            self.reply(404, 0)
        else:
            self.reply(200, 4, **({} if key == "no-etag" else {"ETag": '"original"'}))

    def do_GET(self):
        key = self.path.rsplit("/", 1)[-1]
        if key.startswith("cache-"):
            match = re.fullmatch(r'bytes=(\d+)-(\d+)', self.headers.get('Range', ''))
            assert match and self.headers.get('If-Match') == '"original"'
            start, end = map(int, match.groups())
            assert 0 <= start <= end < object_size
            with reads_lock:
                seen = reads.setdefault(key, [])
                seen.append((start, end))
                number = len(seen)
            if key in ('cache-version','cache-version-large') and start >= 4096:
                self.reply(412, 0)
                return
            if key == 'cache-failure' and number == 1:
                self.reply(503, 0)
                return
            body = bytes(i % 251 for i in range(start, end+1))
            self.reply(206, len(body), ETag='"original"', **{'Content-Range': f'bytes {start}-{end}/{object_size}'})
            self.wfile.write(body[:len(body)//2] if key == "cache-partial" and number == 1 else body)
            return
        if self.headers.get("Range") != "bytes=0-3" or self.headers.get("If-Match") != '"original"':
            self.reply(400, 0)
            return
        error = {"precondition": 412, "deleted": 404, "denied": 403}.get(key)
        if error:
            self.reply(error, 0)
            return
        if key == "ignored-range":
            self.reply(200, 4, ETag='"original"')
            self.wfile.write(b"abcd")
            return
        self.reply(206, 4, **{
            "ETag": '"new"' if key == "wrong-etag" else '"original"',
            "Content-Range": "bytes 0-1/4" if key == "wrong-range" else "bytes 0-3/4",
        })
        self.wfile.write(b"b" if key == "truncated" else b"abcd")


with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        result = subprocess.run([sys.argv[1], f"http://127.0.0.1:{server.server_port}"], timeout=40)
    finally:
        server.shutdown()
        thread.join()
    if result.returncode == 0:
        assert reads['cache-reuse'] == [(0,4095),(1,5000),(4096,8191)], reads['cache-reuse']
        assert reads['cache-boundary'] == [(0,4095),(4096,8191),(81920,81936)], reads['cache-boundary']
        assert reads['cache-parallel'] == [(0,4095)], reads['cache-parallel']
        assert reads['cache-failure'] == [(0,4095),(0,4095)], reads['cache-failure']
        assert reads['cache-partial'] == [(0,4095),(0,4095)], reads['cache-partial']
        assert reads['cache-version-large'] == [(0,4095),(4096,9095)], reads['cache-version-large']
        assert reads['cache-version'] == [(0,4095),(4096,8191)], reads['cache-version']
        eviction = reads['cache-eviction']
        assert len(eviction) == 18 and Counter(eviction)[(4096,8191)] == 2
        assert Counter(eviction)[(0,4095)] == 1, eviction
    sys.exit(result.returncode)
