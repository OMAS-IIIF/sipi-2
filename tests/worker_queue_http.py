"""Worker queue behavior through the real HTTP listener.

1. Queued requests of clients that already disconnected are not rendered.
2. A configured queue limit rejects further waiting requests with 503.

In both scenarios one worker is held by a request whose Lua policy waits for a
local HTTP endpoint (the "gate"). The gate opens only after the queue has been
prepared, so no timing threshold decides the outcome.
"""
import http.server
from pathlib import Path
import shutil
import socket
import sys
import tempfile
import threading
import time
from cache_http import server, get, success

DISCONNECTED = 5
POLICY = """
function preflight(request)
  if request.identifier == 'slow' then
    assert(sipi.http.request{url=sipi.config.gate_url, timeout_ms=10000})
  end
  return {decision='allow', source='tiled.tif'}
end
"""
IMAGE = "/tiled.tif/full/max/0/default.png"


class Gate(http.server.BaseHTTPRequestHandler):
    """Answer /slow only after the test opens the gate."""
    def log_message(self, *args):
        pass

    def do_GET(self):
        self.server.entered.set()
        self.server.release.wait(20)
        self.send_response(200)
        self.send_header("Content-Length", "0")
        self.end_headers()


class HeldWorker:
    """Occupy the single worker until release(); reset the gate for each use."""
    def __init__(self, gate, port):
        gate.entered.clear()
        gate.release.clear()
        self.gate, self.status = gate, []
        self.thread = threading.Thread(target=lambda: self.status.append(get(port, "/slow/info.json")[0]))
        self.thread.start()
        assert gate.entered.wait(10), "slow policy request did not start"

    def release(self):
        self.gate.release.set()
        self.thread.join(20)
        assert not self.thread.is_alive() and self.status == [200], self.status


def observed(log, markers):
    """Wait for explicit I/O lifecycle observations, never socket-order guesses."""
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        output = log.snapshot()
        if all(marker in output for marker in markers):
            return
        time.sleep(.01)
    raise AssertionError(("I/O observations missing", markers, log.snapshot()[-2000:]))


def raw_request(port, path):
    client = socket.create_connection(("127.0.0.1", port), timeout=20)
    client.sendall(f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n".encode())
    return client


def read_status(client):
    with client:
        data = b""
        while chunk := client.recv(65536):
            data += chunk
    return int(data.split(b" ", 2)[1])


def gate_option(gate):
    return f"--preflight-config=gate_url=http://127.0.0.1:{gate.server_port}/slow"


def disconnected_clients(executable, root, gate):
    options = ["--iiif-workers=1", gate_option(gate)]
    with server(executable, root, options, cache=None) as (port, log):
        held = HeldWorker(gate, port)
        clients = [raw_request(port, f"{IMAGE}?n={i}") for i in range(DISCONNECTED)]
        ports = [client.getsockname()[1] for client in clients]
        observed(log, [f"IIIF request queued: peer_port={p}" for p in ports])
        for client in clients:
            client.close()
        observed(log, [f"IIIF connection closed: peer_port={p}" for p in ports])
        held.release()
        assert success(get(port, IMAGE))  # The pool still serves live clients.
    skipped = log[0].count("IIIF client disconnected; skipping remaining work")
    rendered = log[0].count("Resolved IIIF crop")
    assert skipped == DISCONNECTED, (skipped, log[0][-2000:])
    assert rendered == 1, (rendered, log[0][-2000:])


def queue_limit(executable, root, gate):
    options = ["--iiif-workers=1", "--iiif-queue-limit=2", gate_option(gate)]
    with server(executable, root, options, cache=None) as (port, log):
        held = HeldWorker(gate, port)
        waiting = [raw_request(port, IMAGE) for _ in range(2)]
        observed(log, [f"IIIF request queued: peer_port={client.getsockname()[1]}" for client in waiting])
        status, headers, body = get(port, IMAGE)
        headers = {k.lower(): v for k, v in headers.items()}
        assert status == 503 and headers.get("retry-after") == "1", (status, headers)
        assert b"iiif_busy" in body, body
        held.release()
        assert [read_status(client) for client in waiting] == [200, 200]
    assert "IIIF processing pool: 1 workers, queue limit 2" in log[0], log[0][:500]


def main():
    executable, fixtures = (Path(p).resolve() for p in sys.argv[1:])
    gate = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Gate)
    gate.daemon_threads = True
    gate.entered, gate.release = threading.Event(), threading.Event()
    threading.Thread(target=gate.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="sipi-queue-") as temporary:
            root = Path(temporary)
            (root / "images").mkdir()
            shutil.copyfile(fixtures / "tiled.tif", root / "images" / "tiled.tif")
            (root / "policy.lua").write_text(POLICY)
            disconnected_clients(executable, root, gate)
            queue_limit(executable, root, gate)
    finally:
        gate.release.set()
        gate.shutdown()
        gate.server_close()
    print(f"Skipped {DISCONNECTED} disconnected queued requests; queue limit enforced")


if __name__ == "__main__":
    main()
