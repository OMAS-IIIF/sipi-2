"""JWT interoperability oracle using Python HMAC and the independent OpenSSL CLI."""
import base64
import hmac
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time

SECRET = b"s" * 32
NOW = 1800000000


def b64(value):
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode()


def unb64(value):
    return base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))


def js(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode()


def main():
    with tempfile.TemporaryDirectory(prefix="sipi-jwt-") as tmp:
        root = Path(tmp)

        def openssl(*args, data=None):
            return subprocess.run(["openssl", *map(str, args)], input=data, capture_output=True, check=True).stdout

        for prefix, bits in [("", 2048), ("weak-", 1024)]:
            openssl("genpkey", "-algorithm", "RSA", "-pkeyopt", f"rsa_keygen_bits:{bits}", "-out", root / f"{prefix}private.pem")
            openssl("pkey", "-in", root / f"{prefix}private.pem", "-pubout", "-out", root / f"{prefix}public.pem")
        openssl("genpkey", "-algorithm", "EC", "-pkeyopt", "ec_paramgen_curve:P-256", "-out", root / "ec-private.pem")
        openssl("pkey", "-in", root / "ec-private.pem", "-pubout", "-out", root / "ec-public.pem")

        def token(claims=None, header=None, raw=None, raw_header=None, rsa=False, secret=SECRET):
            header = header if header is not None else {"alg": "RS256" if rsa else "HS256", "typ": "JWT"}
            claims = claims if claims is not None else {"exp": NOW + 100}
            data = (b64(raw_header if raw_header is not None else js(header)) + "." +
                    b64(raw if raw is not None else js(claims))).encode()
            signature = openssl("dgst", "-sha256", "-sign", root / "private.pem", data=data) if rsa else hmac.digest(secret, data, "sha256")
            return data.decode() + "." + b64(signature)

        cases = []

        def case(name, value, error="", **kwargs):
            cases.append(dict(name=name, token=value, error=error, **kwargs))

        nested = {"exp": NOW + 100, "nested": [None, False, {}, [], {"name": "é\0😀", "id": 9223372036854775807}]}
        case("nested", token(nested), claims=nested)
        case("rsa", token(rsa=True), algorithm="RS256")
        rsa_parts = token(rsa=True).split('.')
        rsa_parts[1] = b64(js({"exp": NOW+100, "admin": True}))
        case("rsa changed payload", '.'.join(rsa_parts), "signature", algorithm="RS256")
        case("rsa policy rejects hmac", token(), "algorithm", algorithm="RS256")
        case("issuer/audience", token({"exp": NOW + 100, "iss": "issuer", "aud": ["a", "b"]}), issuer="issuer", audience="b", type="JWT")
        case("aud string", token({"exp": NOW + 100, "aud": "a"}), audience="a")
        case("audience omitted", token({"exp": NOW + 100, "aud": "a"}), "claims")
        case("wrong audience", token({"exp": NOW + 100, "aud": ["a"]}), "claims", audience="b")
        case("missing audience", token(), "claims", audience="b")
        case("missing issuer", token(), "claims", issuer="b")
        case("wrong issuer", token({"exp": NOW + 100, "iss": "a"}), "claims", issuer="b")
        case("wrong type", token(), "claims", type="at+jwt")
        case("missing type", token(header={"alg": "HS256"}), "claims", type="JWT")
        case("type optional", token(header={"alg": "HS256"}))
        case("post2038", token({"exp": 2200000100}), now=2200000000)
        case("max int64 exp", token({"exp": 9223372036854775807}))
        for exp, leeway, error in [(NOW, 0, "expired"), (NOW-1, 0, "expired"), (NOW+1, 0, ""),
                                    (NOW, 1, ""), (NOW-1, 1, "expired")]:
            case(f"expiry boundary {exp}/{leeway}", token({"exp": exp}), error, leeway=leeway)
        for field in ["nbf", "iat"]:
            for offset, leeway, error in [(0, 0, ""), (1, 0, "not_yet_valid"), (1, 1, "")]:
                case(f"{field} boundary {offset}/{leeway}", token({"exp": NOW+100, field: NOW+offset}), error, leeway=leeway)
            for value in [None, "123", 1.5, -1, True]:
                case(f"{field} wrong type {value}", token({"exp": NOW+100, field: value}), "claims")
        for value in [None, "123", 1.0, 1.5, -1, True]:
            case(f"exp wrong type {value}", token({"exp": value}), "claims")
        case("no expiry", token({}), "claims")
        case("explicit no expiry", token({}), require_exp=False)
        case("expired still checked", token({"exp": NOW}), "expired", require_exp=False)
        for name, value in [("iss", 1), ("sub", False), ("jti", []), ("aud", []), ("aud", ["a", None]), ("aud", "")]:
            case(f"claim type {name}/{value}", token({"exp": NOW+100, name: value}), "claims")
        for header in [{"alg": "none"}, {"alg": "HS512"}, {"alg": "RS256"}, {"alg": 1}]:
            case("algorithm confusion", token(header=header), "algorithm")
        for field in ["crit", "b64", "jku", "jwk", "x5u", "x5c", "zip", "enc"]:
            case(f"header extension {field}", token(header={"alg": "HS256", field: []}), "malformed")
        case("bad header type", token(header={"alg": "HS256", "typ": 123}), "malformed")
        case("bad signature", token(secret=b"t"*32), "signature")
        for raw in [b'{"exp":1,"exp":1800000100}', b'{"exp":01}', b'{"exp":1.}', b'{"exp":NaN}',
                    b'{"exp":184467440737095516160}', b'{"exp":1800000100} trailing', b'{"x":"\xff"}',
                    b'[]', b'null', b'123', b'"text"', b'{"exp":1800000100,"x":'+b'['*70+b'0'+b']'*70+b'}']:
            case("strict payload", token(raw=raw), "malformed")
        case("duplicate header", token(raw_header=b'{"alg":"none","alg":"HS256"}'), "malformed")
        good = token()
        alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"
        noncanonical = good[:-1] + alphabet[alphabet.index(good[-1]) + 1]
        for value in ["", "a.b", good+".x", good+"=", " "+good, good+"\0", noncanonical, "x"*16385]:
            case("compact encoding", value, "malformed")
        (root / "cases.json").write_text(json.dumps(cases))
        public = (root / "public.pem").read_text()
        private = (root / "private.pem").read_text()
        # PEM is embedded as a Lua long string; test keys never leave the temporary directory.
        (root / "script.lua").write_text('''
local j=sipi.jwt
local secret=string.rep('s',32)
local opts={algorithm='HS256',key=secret,issuer='issuer',audience='service',type='JWT'}
local external=''' + json.dumps(token({"exp": int(time.time())+600, "iss": "issuer", "aud": "service"})) + '''
local public=[=[''' + public + ''']=]
local private=[=[''' + private + ''']=]
assert(j.verify(external,opts).iss=='issuer') -- available during initialization
function preflight()
    local claims,err=j.verify(external,opts); assert(claims and err==nil)
    local t=j.sign({iss='issuer',aud={'service','other'},detail={sipi.json.null,false},id=math.maxinteger},
                  {algorithm='HS256',key=secret,expires_in=60,kid='current'})
    local c=assert(j.verify(t,opts)); assert(c.id==math.maxinteger and c.detail[1]==sipi.json.null)
    assert(c.exp-c.iat==60 and c.detail[2]==false)
    local rsa=j.sign({exp=c.exp,iat=c.iat},{algorithm='RS256',key=private})
    assert(j.verify(rsa,{algorithm='RS256',key=public}).exp==c.exp)
    local no,e=j.verify('invalid',opts); assert(no==nil and e.code=='malformed')
    local no,e=j.verify(string.rep('x',16385),opts); assert(no==nil and e.code=='malformed')
    local function bad(fn,...) assert(not pcall(fn,...)) end
    bad(j.sign,{}, {algorithm='HS256',key='short',expires_in=60})
    bad(j.sign,{}, {algorithm='HS256',key=secret})
    bad(j.sign,{exp=1}, {algorithm='HS256',key=secret,expires_in=60})
    bad(j.sign,{}, {algorithm='none',key=secret,expires_in=60})
    bad(j.sign,{}, {algorithm='HS256',key=secret,expires_in=0})
    bad(j.sign,{}, {algorithm='HS256',key=secret,expires_in=1.5})
    bad(j.sign,{exp='123'}, {algorithm='HS256',key=secret})
    bad(j.sign,{exp=1,x=string.rep('x',11000)}, {algorithm='HS256',key=secret})
    bad(j.sign,sipi.json.array(), {algorithm='HS256',key=secret,expires_in=60})
    bad(j.verify,external, {algorithm='HS256',key=secret,leeway=301})
    bad(j.verify,external, {algorithm='HS256',key=secret,require_exp='yes'})
    bad(j.verify,external, {algorithm='HS256',key=secret,expires_in=30})
    bad(j.verify,external, setmetatable(opts,{})); setmetatable(opts,nil)
    bad(j.verify,external, {algorithm='HS256',key=secret,unknown=true})
    bad(j.verify,false,opts)
    bad(j.verify,external,opts,'extra')
    assert(j.verify(external,opts)) -- recovery after argument errors
    return {decision='allow'}
end
''')
        (root / "budget.lua").write_text('''
function preflight()
    local options={algorithm='HS256',key=string.rep('s',32)}
    for i=1,32 do local r,e=sipi.jwt.verify('invalid',options); assert(r==nil and e.code=='malformed') end
    local ok,err=pcall(sipi.jwt.verify,'invalid',options)
    assert(not ok and string.find(err,'budget'))
    assert(not pcall(sipi.jwt.sign,{exp=1},options)) -- shared budget
    return {decision='allow'}
end
''')
        # Force protected native-to-Lua conversion to fail, then recover in the same state.
        pressure_token = token({"exp": int(time.time())+600, "values": [{} for _ in range(2500)]})
        (root / "pressure.lua").write_text('''
local token='''+json.dumps(pressure_token)+'''
function preflight()
    local opts={algorithm='HS256',key=string.rep('s',32)}
    local held={}
    while pcall(function() held[#held+1]=string.rep('x',10000) end) do end
    assert(not pcall(sipi.jwt.verify,token,opts))
    held=nil; collectgarbage('collect')
    assert(#assert(sipi.jwt.verify(token,opts)).values==2500)
    return {decision='allow'}
end
''')
        subprocess.run([sys.argv[1], str(root)], check=True, timeout=30)
        for algorithm in ["HS256", "RS256"]:
            token_value = (root / f"{algorithm}.jwt").read_text()
            header, payload, signature = token_value.split('.')
            data = (header+'.'+payload).encode()
            if algorithm == "HS256":
                assert hmac.compare_digest(unb64(signature), hmac.digest(SECRET, data, "sha256"))
            else:
                (root / "signature.bin").write_bytes(unb64(signature))
                openssl("dgst", "-sha256", "-verify", root / "public.pem", "-signature", root / "signature.bin", data=data)
            assert json.loads(unb64(header)) == {"alg": algorithm, "typ": "JWT", "kid": "test-key"}
            claims = json.loads(unb64(payload))
            assert claims["exp"] == 2200000300 and claims["iat"] == 2200000000
            assert claims["large"] == 9223372036854775807 and claims["nested"]["items"] == [None, False]
        print("Independent HS256/RS256 signing interoperability passed")


if __name__ == '__main__':
    main()
