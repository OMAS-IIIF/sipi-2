"""Offline HTTP/TLS contract tests through the real LuaPreflight public API."""
import http.server
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        pass  # Expected disconnects for bounded-response and timeout probes.


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def do_GET(self):
        if self.path == "/slow":
            time.sleep(0.25)
        if self.path == "/deadline":
            time.sleep(4)
        if self.path == "/target":
            self.server.redirect_hits += 1
        if self.path == "/interim":
            self.wfile.write(b"HTTP/1.1 103 Early Hints\r\nX-Interim: ignored\r\n\r\n")
        if self.path == "/trailers":
            self.wfile.write(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTrailer: X-Trailer\r\n\r\n3\r\nabc\r\n0\r\nX-Trailer: one\r\nX-Trailer: two\r\n\r\n")
            return
        if self.path == "/partial":
            self.wfile.write(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\nConnection: close\r\n\r\nshort")
            self.close_connection = True
            return
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        if self.path == "/binary":
            output = body
        elif self.path == "/medium":
            output = b"x" * 900000
        elif self.path == "/large":
            output = b"x" * (1024 * 1024 + 1)
        else:
            output = json.dumps({"method": self.command, "body": body.decode(),
                                 "auth": self.headers.get("Authorization"),
                                 "cookie": self.headers.get("Cookie"),
                                 "empty": self.headers.get("X-Empty"),
                                 "many": self.headers.get_all("X-Many"),
                                 "connection": self.client_address[1]}).encode()
        status = 403 if self.path == "/denied" else 302 if self.path == "/redirect" else 200
        self.send_response(status)
        self.send_header("Content-Length", str(len(output)))
        self.send_header("Content-Type", "application/json")
        self.send_header("Set-Cookie", "a=1")
        self.send_header("Set-Cookie", "b=2")
        if self.path == "/redirect":
            self.send_header("Location", "/target")
        if self.path == "/headers":
            self.send_header("X-Large", "x" * 65536)
        if self.path == "/count":
            for i in range(129):
                self.send_header(f"X-{i}", "v")
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(output)

    do_POST = do_PUT = do_PATCH = do_DELETE = do_OPTIONS = do_HEAD = do_GET


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="sipi-lua-http-") as tmp:
        directory = Path(tmp)
        cert, key = directory / "cert.pem", directory / "key.pem"
        subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes",
                        "-keyout", str(key), "-out", str(cert), "-days", "1", "-subj", "/CN=localhost",
                        "-addext", "subjectAltName=DNS:localhost"], check=True, capture_output=True)
        http = Server(("127.0.0.1", 0), Handler)
        tls = Server(("127.0.0.1", 0), Handler)
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(cert, key)
        tls.socket = context.wrap_socket(tls.socket, server_side=True)
        http.redirect_hits = tls.redirect_hits = 0
        servers = [http, tls]
        for server in servers:
            threading.Thread(target=server.serve_forever, daemon=True).start()
        base = f"http://127.0.0.1:{http.server_port}"
        secure = f"https://localhost:{tls.server_port}"
        checks = 0

        def run(code, parallel=1, top="", success=True):
            nonlocal checks
            script = directory / "test.lua"
            script.write_text(f"local base={json.dumps(base)}\nlocal secure={json.dumps(secure)}\n"
                              f"local ca={json.dumps(str(cert))}\nlocal http=sipi.http.request\nlocal json=sipi.json\n"
                              + top + "\nfunction preflight(request)\n" + code + "\nreturn {decision='allow'}\nend\n")
            env = dict(os.environ, http_proxy="http://127.0.0.1:1", https_proxy="http://127.0.0.1:1",
                       ALL_PROXY="http://127.0.0.1:1", NO_PROXY="")
            result = subprocess.run([binary, str(script), str(parallel)], env=env,
                                    capture_output=True, text=True, timeout=25)
            assert (result.returncode == 0) == success, result.stderr + result.stdout
            checks += 1

        try:
            run('''
                local a, err = http{url=base..'/echo'}
                assert(a and err == nil and a.status == 200)
                local v = json.decode(a.body)
                assert(v.method == 'GET' and v.auth == json.null and v.cookie == json.null)
                assert(a.headers['set-cookie'][1] == 'a=1' and a.headers['set-cookie'][2] == 'b=2')
                local b = assert(http{url=base..'/echo', method='POST', body=json.encode({a=1}),
                    headers={Authorization='Bearer explicit', ['Content-Type']='application/json',
                             ['X-Empty']='', ['X-Many']={'one','two'}}})
                local w = json.decode(b.body)
                assert(w.method=='POST' and json.decode(w.body).a==1 and w.auth=='Bearer explicit')
                assert(w.empty=='' and w.many[2]=='two' and w.connection==v.connection)
                local c = json.decode(assert(http{url=base..'/echo'}).body)
                assert(c.auth==json.null and c.cookie==json.null and c.body=='')
                local d = assert(http{url=base..'/binary',method='PUT',body='a'..string.char(0)..'b'})
                assert(d.body=='a'..string.char(0)..'b')
                local absent, exhausted = http{url=base..'/echo'}
                assert(absent==nil and exhausted.code=='budget')
            ''', parallel=4)
            for method in ["HEAD", "PATCH", "DELETE", "OPTIONS", "POST"]:
                run(f"local r=assert(http{{url=base..'/echo',method='{method}'}}); assert(r.status==200)\n"
                    + ("assert(r.body=='')" if method == "HEAD" else f"assert(json.decode(r.body).method=='{method}')"))
            run('''
                local r,e=http{url=base..'/denied'}; assert(r.status==403 and e==nil)
                local redirect=assert(http{url=base..'/redirect',headers={Authorization='secret'}})
                assert(redirect.status==302 and redirect.headers.location[1]=='/target')
                local interim=assert(http{url=base..'/interim'})
                assert(interim.status==200 and interim.headers['x-interim']==nil)
                local trailer=assert(http{url=base..'/trailers'})
                assert(trailer.body=='abc' and trailer.headers['x-trailer']==nil)
                assert(trailer.trailers['x-trailer'][2]=='two')
            ''')
            assert http.redirect_hits == 0
            run('''
                for _,path in ipairs({'/large','/headers','/count'}) do
                    local r,e=http{url=base..path}; assert(r==nil and e.code=='response_limit',e and e.code)
                end
                local r,e=http{url=base..'/echo',max_response_bytes=3}
                assert(r==nil and e.code=='response_limit')
            ''')
            run('''
                local r,e=http{url=base..'/slow',timeout_ms=30}; assert(r==nil and e.code=='timeout')
                local r,e=http{url=base..'/partial'}; assert(r==nil and e.code=='network')
                assert(http{url=base..'/echo'})
            ''')
            run('''
                local r,e=http{url=secure..'/echo'}; assert(r==nil and e.code=='tls')
                assert(http{url=secure..'/echo',ca_file=ca})
                local r,e=http{url=string.gsub(secure,'localhost','127.0.0.1')..'/echo',ca_file=ca}
                assert(r==nil and e.code=='tls')
                local r,e=http{url=secure..'/echo'}; assert(r==nil and e.code=='tls')
            ''')
            run('''
                local function bad(o) assert(not pcall(http,o)) end
                bad({}); bad({url='file:///etc/passwd'}); bad({url=base,method='CONNECT'})
                bad({url=base,verify_tls=false}); bad({url=base,timeout_ms=10001})
                bad({url=base,connect_timeout_ms=2001}); bad({url=base,max_response_bytes=0})
                bad({url=base,timeout_ms=1.5}); bad({url=base,method='HEAD',body='x'})
                bad({url=base..'#fragment'}); bad({url='http://user:secret@localhost/'})
                bad({url=base..string.char(0)..'hidden'}); bad({url=base,body=12})
                bad({url=base,body=string.rep('x',1048577)})
                bad({url=base,headers={Host='attacker'}})
                bad({url=base,headers={['Content-Length']='123'}})
                bad({url=base,headers={['Transfer-Encoding']='chunked'}})
                bad({url=base,headers={['X-Test']='a\\r\\nAuthorization: secret'}})
                bad({url=base,headers={['Bad:Name']='x'}})
                bad({url=base,headers={Foo='a',foo='b'}})
                bad({url=base,headers={Foo={[2]='hole'}}})
                bad({url=base,headers={Foo={}}})
                bad(setmetatable({url=base},{__index=function() error('must not run') end}))
                assert(not pcall(http,{url=base},'extra'))
                assert(http{url=base..'/echo'}) -- validation failures consume no network budget
            ''')
            run('''
                local held={}
                while pcall(function() held[#held+1]=string.rep('x',200000) end) do end
                assert(not pcall(http,{url=base..'/medium'}))
                held=nil; collectgarbage('collect')
                assert(#assert(http{url=base..'/medium'}).body==900000)
            ''')
            run("", top="http{url=base..'/target'}", success=False)
            assert http.redirect_hits == 0
            finalizer = "kept=setmetatable({}, {__gc=function() http{url=base..'/target'} end})"
            run(finalizer)
            run(finalizer + "\nerror('intentional failure')", success=False)
            assert http.redirect_hits == 0
            started = time.monotonic()
            run('''
                for i=1,4 do
                    local r,e=http{url=base..'/deadline',timeout_ms=3000}
                    assert(r==nil and (e.code=='timeout' or e.code=='budget'))
                end
                local r,e=http{url=base..'/echo'}; assert(r==nil and e.code=='budget')
            ''')
            assert time.monotonic() - started < 13
            with socket.socket() as closed:
                closed.bind(("127.0.0.1", 0))
                unused_port = closed.getsockname()[1]
            run(f"local r,e=http{{url='http://127.0.0.1:{unused_port}/'}}; assert(r==nil and e.code=='network')")
            print(f"{checks} Lua HTTP/TLS scenarios passed")
        finally:
            for server in servers:
                server.shutdown()
                server.server_close()


if __name__ == "__main__":
    main()
