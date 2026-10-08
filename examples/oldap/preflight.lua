-- OLDAP deployment policy; the server's Lua API remains application-independent.
-- All settings are startup snapshots. No network calls occur during validation.
local config, secrets = sipi.config, sipi.secrets
local issuer = config.jwt_issuer or "https://oldap.org"
local audience = config.jwt_audience or "oldap-api-media"
assert(type(secrets.media_jwt_key) == "string" and #secrets.media_jwt_key >= 32,
       "OLDAP requires a media signing key of at least 32 bytes")
local minimum = tonumber(config.minimum_permission)
assert(minimum and minimum % 1 == 0 and minimum >= 1 and minimum <= 6,
       "OLDAP minimum_permission must be explicitly set to 1..6")
-- Zero disables permission reuse; positive lifetimes never slide on a hit.
local permission_ttl = tonumber(config.anonymous_permission_ttl or "600")
assert(permission_ttl and permission_ttl % 1 == 0 and permission_ttl >= 0 and permission_ttl <= 3600,
       "OLDAP anonymous_permission_ttl must be an integer in 0..3600 seconds")
local unknown_user = config.unknown_user or "unknown"
local api = config.api_url
if api then
    assert(api:match("^https?://[^/%s?#@]+") and not api:find("[?#%s@]"), "Invalid OLDAP API URL")
    api = api:gsub("/+$", "")
    -- OLDAP's anonymous pseudo-user deliberately has no password. A different
    -- service account must still provide its credential explicitly.
    if unknown_user ~= "unknown" then
        assert(type(secrets.unknown_password) == "string" and #secrets.unknown_password > 0,
               "A non-anonymous service account requires unknown_password")
    end
end
local cookie_name = config.media_cookie -- Explicit opt-in; not a normal OLDAP login cookie.

-- Reject ambiguous arrays; OLDAP single-valued fields can be strings or singleton arrays.
local function scalar(value)
    if type(value) == "table" then
        if #value ~= 1 then return nil end
        for key in pairs(value) do if key ~= 1 then return nil end end
        value = value[1]
    end
    if type(value) == "string" then return value end
end

-- A component cannot escape the repository or change its spelling through cleanup.
local function component(value)
    return type(value) == "string" and value ~= "" and value ~= "." and value ~= ".."
        and not value:find("[/\\%c]")
end

-- Return a relative local source; SourceResolver additionally enforces root/symlink containment.
local function source(identifier, path, filename)
    path, filename = scalar(path), scalar(filename)
    if not component(identifier) or not component(filename) or not path then return nil end
    if path:find("[\\%c]") or path:sub(1, 1) == "/" or path:sub(-1) == "/"
        or path:find("//", 1, true) then return nil end
    for part in path:gmatch("[^/]+") do
        if not component(part) then return nil end
    end
    local prefix = path == "" and "" or path .. "/"
    return prefix .. identifier .. "/derived/" .. filename
end

-- Percent-encode one URL path component; incoming identifier is already decoded.
local function segment(value)
    return (value:gsub("([^A-Za-z0-9._~-])", function(c)
        return string.format("%%%02X", string.byte(c))
    end))
end

-- Priority: query token, Authorization Bearer, optional named media cookie.
-- A malformed higher-priority credential never falls through to weaker credentials.
local function credential(request)
    local values = request.query.token
    if values then
        if #values ~= 1 or values[1] == "" then return nil, true end
        return values[1], true
    end
    local header = request.headers.authorization
    if header then
        return header:match("^[Bb][Ee][Aa][Rr][Ee][Rr] ([^%s]+)$"), true
    end
    if cookie_name and request.cookies[cookie_name] ~= nil then
        local value = request.cookies[cookie_name]
        return value ~= "" and value or nil, true
    end
    return nil, false
end

-- Transport/protocol errors are technical failures (500), never an authorization grant.
local function json_request(options, lookup)
    options.timeout_ms = 3000
    options.connect_timeout_ms = 1000
    options.max_response_bytes = 65536
    options.ca_file = config.ca_file
    local response = sipi.http.request(options)
    if not response then error("OLDAP service unavailable") end
    if lookup and (response.status == 401 or response.status == 403 or response.status == 404) then
        return nil, response.status
    end
    if response.status ~= 200 then error("Unexpected OLDAP service status") end
    local ok, result = pcall(sipi.json.decode, response.body)
    if not ok or type(result) ~= "table" then error("Invalid OLDAP service response") end
    return result, response.status
end

-- Login and permission keys share this immutable policy/configuration snapshot.
-- Endpoint, account, threshold and secrets cannot change within that snapshot.
local login_cache_key = "oldap.anonymous-login"
local function login_unknown()
    local login = json_request {
        url = api .. "/admin/auth/" .. segment(unknown_user), method = "POST",
        headers = {["Content-Type"] = "application/json", Accept = "application/json"},
        body = sipi.json.encode {password = secrets.unknown_password or ""}
    }
    local access_token = login.accessToken
    if access_token == nil then access_token = login.token end
    if type(access_token) ~= "string" or access_token == "" or access_token:find("%c") then
        error("Invalid OLDAP login response")
    end
    -- Use the API's authenticated lifetime, not unverified JWT claims. Retain at
    -- most ten minutes, subtract five seconds, and include HTTP time in that TTL.
    local lifetime = login.expiresIn
    if type(lifetime) ~= "number" or lifetime % 1 ~= 0 or lifetime <= 5 then
        error("Invalid OLDAP login lifetime")
    end
    return access_token, math.min(600, lifetime - 5)
end

-- Validate before caching: retain only the allow decision and resolved source,
-- never arbitrary REST metadata or a partially checked path.
local function decision(object, path, filename, identifier)
    if not object or type(object.permval) ~= "number" or object.permval % 1 ~= 0
        or object.permval < minimum or object.permval > 6 then
        return {decision = "deny"}
    end
    local selected = source(identifier, path, filename)
    if not selected then return {decision = "deny"} end
    return {decision = "allow", source = selected}
end

-- Acquire login outside the permission loader: recursive cache loads are forbidden
-- to prevent cross-worker deadlocks. A transient 401 outcome unwinds the flight
-- before revision-safe login renewal. Denials are shared, but never retained.
local function anonymous_decision(identifier)
    if not api then return {decision = "deny"} end
    local key = "oldap.anonymous-permission:" .. identifier
    -- Long valid identifiers remain usable without exceeding the generic key limit.
    local cacheable = permission_ttl > 0 and #key <= 256
    if cacheable then
        local cached = sipi.cache.get(key)
        if cached then return cached end
    end
    for attempt = 1, 2 do
        local access_token, revision = sipi.cache.get_or_load(login_cache_key, login_unknown)
        local function lookup()
            local object, status = json_request({
                url = api .. "/data/mediaobject/id/" .. segment(identifier),
                headers = {Authorization = "Bearer " .. access_token, Accept = "application/json"}
            }, true)
            if status == 401 then
                return {retry_login = true, login_revision = revision}, 0
            end
            local result = decision(object, object and object["shared:path"],
                                    object and object["shared:derivativeName"], identifier)
            return result, result.decision == "allow" and permission_ttl or 0
        end
        local result
        if cacheable then result = sipi.cache.get_or_load(key, lookup)
        else result = lookup() end
        if not result.retry_login then return result end
        -- A waiter may have joined a loader using another login generation.
        sipi.cache.invalidate(login_cache_key, result.login_revision)
    end
    return {decision = "deny"}
end

-- All IIIF kinds and GET/HEAD share the same policy; prefixes are routing aliases,
-- not OLDAP project namespaces. OLDAP asset identifiers must be globally unique.
function preflight(request)
    if not component(request.identifier) then return {decision = "deny"} end
    local token, supplied = credential(request)
    local object, path, filename
    if supplied then
        if not token then return {decision = "deny"} end
        object = sipi.jwt.verify(token, {
            algorithm = "HS256", key = secrets.media_jwt_key,
            issuer = issuer, audience = audience
        })
        if not object or object.typ ~= "media" or object.id ~= request.identifier then
            return {decision = "deny"}
        end
        if object.assetId ~= nil and object.assetId ~= request.identifier then
            return {decision = "deny"}
        end
        path, filename = object.path, object.derivativeName
    else
        local result = anonymous_decision(request.identifier)
        -- Cache objects have JSON type tags; the preflight contract requires a
        -- plain result table, so explicitly copy its two supported fields.
        return {decision = result.decision, source = result.source}
    end
    return decision(object, path, filename, request.identifier)
end
