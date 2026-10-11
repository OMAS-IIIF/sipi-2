"""Real-process configuration/secret propagation and diagnostic redaction tests."""
import base64
import hmac
import http.client
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import time


SECRET = "sensitive-secret-for-config-tests-123456"


def main():
    binary = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="sipi-script-config-") as tmp:
        root = Path(tmp)
        (root / "images").mkdir()
        secret_file = root / "secret"
        secret_file.write_text(SECRET)
        script = root / "policy.lua"
        script.write_text('''
assert(sipi.config.api_url=='https://cli.example')
assert(sipi.config.issuer=='issuer' and sipi.config.audience=='image-service')
assert(sipi.config.file_only=='file-value' and sipi.config.empty=='')
assert(sipi.config.sensitive_unrelated==nil and sipi.secrets.sensitive_unrelated==nil)
assert(os==nil)
function preflight(request)
    sipi.config.calls=(sipi.config.calls or 0)+1; assert(sipi.config.calls==1)
    local token=string.match(request.headers.authorization or '', '^Bearer (.+)$')
    local claims=assert(sipi.jwt.verify(token, {algorithm='HS256',key=sipi.secrets.jwt_key,
        issuer=sipi.config.issuer,audience=sipi.config.audience}))
    assert(claims.image==request.identifier)
    return {decision='deny'} -- Successful policy check, no fixture image needed.
end
''')
        config = root / "settings.toml"
        config.write_text('''
preflight-script="policy.lua"
iiif-root="images"
preflight-config=["api_url=https://file.example","issuer=issuer","audience=wrong",
                  "file_only=file-value","empty="]
preflight-secret-file=["jwt_key=missing-overridden-secret"]
''')
        env = {k: v for k, v in os.environ.items() if not k.startswith("SIPI_")}
        env.update(SIPI_PREFLIGHT_CONFIG_API_URL="https://env.example",
                   SIPI_PREFLIGHT_CONFIG_AUDIENCE="image-service",
                   SIPI_PREFLIGHT_SECRETS_JWT_KEY=SECRET,
                   SENSITIVE_UNRELATED=SECRET)
        cli = ["--config", "settings.toml", "--preflight-config", "api_url=https://cli.example"]
        checks = 0

        def request(port, path, token=None):
            client = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
            try:
                client.request("GET", path, headers={"Authorization": "Bearer "+token, "Cookie": "session=cookie-sensitive-value"} if token else {})
                response = client.getresponse()
                return response.status, response.read()
            finally:
                client.close()

        def startup_failure(args, environment):
            nonlocal checks
            result = subprocess.run([binary, *args], cwd=root, env=environment, capture_output=True, text=True, timeout=5)
            assert result.returncode == 2, result
            assert SECRET not in result.stdout + result.stderr
            checks += 1

        def run_server(args, environment, runtime_error=False):
            nonlocal checks
            with socket.socket() as probe:
                probe.bind(("127.0.0.1", 0)); port = probe.getsockname()[1]
            process = subprocess.Popen([binary, *args, "--port", str(port)], cwd=root, env=environment,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            try:
                deadline = time.monotonic()+8
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        out, err = process.communicate()
                        raise AssertionError(f"Server failed: {out} {err}")
                    try:
                        if request(port, "/health")[0] == 200:
                            break
                    except (ConnectionError, OSError):
                        time.sleep(0.02)
                else:
                    raise AssertionError("Server did not start")
                # Changing a file after startup must not change the retained secret.
                secret_file.write_text("changed-after-startup")
                encode = lambda v: base64.urlsafe_b64encode(json.dumps(v, separators=(",", ":")).encode()).rstrip(b"=")
                data = encode({"alg":"HS256","typ":"JWT"})+b"."+encode({"exp":int(time.time())+300,
                         "iss":"issuer","aud":"image-service","image":"id"})
                token = (data+b"."+base64.urlsafe_b64encode(hmac.digest(SECRET.encode(),data,"sha256")).rstrip(b"=")).decode()
                for _ in range(2):
                    status, body = request(port, "/id/info.json?token=query-sensitive-value", token)
                    assert status == (500 if runtime_error else 403), (status, body)
                    assert SECRET.encode() not in body
                checks += 1
            finally:
                process.send_signal(signal.SIGTERM) if process.poll() is None else None
                out, err = process.communicate(timeout=5)
                assert SECRET not in out + err
                if runtime_error:
                    assert token not in out + err
                    assert "cookie-sensitive-value" not in out + err
                    assert "query-sensitive-value" not in out + err
                    assert "script_execution; category 2" in out + err
                    assert "policy.lua:" in out + err
                    assert "diagnostic marker" in out + err
                    assert "[redacted]" in out + err
                    assert r"\x0a" in out + err

        run_server(cli, env)
        file_env = dict(env)
        del file_env["SIPI_PREFLIGHT_SECRETS_JWT_KEY"]
        file_env["SIPI_PREFLIGHT_SECRET_FILES_JWT_KEY"] = "secret"
        secret_file.write_text(SECRET)
        run_server(cli, file_env)
        # A CLI file entry overrides a lower-priority literal under the same name.
        secret_file.write_text(SECRET)
        run_server(cli+["--preflight-secret-file", "jwt_key=secret"], dict(env, SIPI_PREFLIGHT_SECRETS_JWT_KEY="wrong"))
        startup_failure(cli, dict(env, SIPI_PREFLIGHT_SECRET_FILES_JWT_KEY="secret")) # Same-level conflict.
        startup_failure(cli, dict(file_env, SIPI_PREFLIGHT_SECRET_FILES_JWT_KEY="missing"))
        startup_failure(["--preflight-config", "key="+SECRET], {}) # Settings without a script.
        startup_failure(["--help", "--preflight-secret", "key="+SECRET,"--preflight-secret","key="+SECRET], {})
        help_result = subprocess.run([binary,"--help","--preflight-secret","key="+SECRET], cwd=root,
            env=dict(env,SIPI_PREFLIGHT_SECRETS_JWT_KEY=""),capture_output=True,text=True,timeout=5)
        assert help_result.returncode==0 and SECRET not in help_result.stdout+help_result.stderr
        script.write_text("error(sipi.secrets.jwt_key)\nfunction preflight() return {decision='allow'} end")
        startup_failure(cli, env)
        script.write_text("""function preflight(request)
    sipi.cache.get_or_load('failing-loader', function()
    error('diagnostic marker\\n' .. sipi.secrets.jwt_key .. ' ' ..
        request.headers.authorization .. ' ' .. request.cookies.session .. ' ' ..
        request.query.token[1])
    end)
end""")
        run_server(cli, env, runtime_error=True)
        print(f"{checks} startup/runtime script configuration and redaction scenarios passed")


if __name__ == "__main__":
    main()
