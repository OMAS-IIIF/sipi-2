-- Example policy: stable public coordinates and a separately protected original.
-- All other prefixes/aliases are denied. Default repository mapping ignores prefix.
-- Add project-specific JWT/REST authorization before enabling the original branch.
function preflight(request)
    if #request.prefix ~= 1 then
        return { decision = "deny" }
    end
    if request.prefix[1] == "public" then
        return {
            decision = "allow",
            resolution = { max_width = 1000, max_height = 1000 }
        }
    end
    -- For an authorized /original/ request, return { decision = "allow" }.
    -- Keep denial as the default; a URL prefix is never proof of authorization.
    return { decision = "deny" }
end
