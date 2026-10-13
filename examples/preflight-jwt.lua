-- Example application contract: a bearer JWT authorizes /sample.png only.
-- Required settings: jwt_issuer, jwt_audience; secret: jwt_key (raw HS256 key).
-- Input: decoded request with optional Authorization header. Output: allow/deny.
-- No query/cookie fallback, external calls, writes or persistent state.
-- See docs/preflight.md for configuration, claims and test commands.
local issuer = assert(sipi.config.jwt_issuer, "jwt_issuer is required")
local audience = assert(sipi.config.jwt_audience, "jwt_audience is required")
local key = assert(sipi.secrets.jwt_key, "jwt_key is required")
assert(issuer ~= "" and audience ~= "", "JWT settings must not be empty")

function preflight(request)
    -- Restrict the namespace before inspecting credentials. Prefix aliases would
    -- otherwise resolve to the same file under the default repository mapping.
    if #request.prefix ~= 0 or request.identifier ~= "sample.png" then
        return { decision = "deny" }
    end
    local header = request.headers.authorization
    local token = header and header:match("^[Bb][Ee][Aa][Rr][Ee][Rr] +([^%s]+)$")
    if not token then
        return { decision = "deny" }
    end
    local claims = sipi.jwt.verify(token, {
        algorithm = "HS256", key = key, issuer = issuer,
        audience = audience, type = "JWT"
    })
    -- Signature/expiry/issuer/audience checks are handled by the helper. Resource
    -- binding and the boolean read permission are this example's own contract.
    if not claims or claims.sub ~= request.identifier or claims.read ~= true then
        return { decision = "deny" }
    end
    return { decision = "allow" }
end
