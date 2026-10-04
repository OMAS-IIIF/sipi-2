"""Serve controlled S3-shaped replies to verify range and failure handling offline."""
import http.server
import subprocess
import sys
import threading


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
        if key == "missing":
            self.reply(404, 0)
        else:
            self.reply(200, 4, **({} if key == "no-etag" else {"ETag": '"original"'}))

    def do_GET(self):
        key = self.path.rsplit("/", 1)[-1]
        if self.headers.get("Range") != "bytes=1-2" or self.headers.get("If-Match") != '"original"':
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
        self.reply(206, 2, **{
            "ETag": '"new"' if key == "wrong-etag" else '"original"',
            "Content-Range": "bytes 0-1/4" if key == "wrong-range" else "bytes 1-2/4",
        })
        self.wfile.write(b"b" if key == "truncated" else b"bc")


with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        result = subprocess.run([sys.argv[1], f"http://127.0.0.1:{server.server_port}"], timeout=40)
    finally:
        server.shutdown()
        thread.join()
    sys.exit(result.returncode)
