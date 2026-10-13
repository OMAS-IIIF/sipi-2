-- Minimal native policy: expose the bundled image only through /example.
-- Run from the repository root with examples/native/sipi.toml and
-- --preflight-script examples/preflight.lua (see docs/preflight.md).
-- Input: a decoded IIIF request snapshot. Output: one allow/deny table.
-- All routes and methods use the same policy; no credentials or external I/O.
function preflight(request)
    if #request.prefix == 0 and request.identifier == "example" then
        return { decision = "allow", source = "sample.png" }
    end
    return { decision = "deny" }
end
