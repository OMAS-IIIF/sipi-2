-- Public documentation policy: allow only the bundled sample image.
-- Input: the standard preflight request snapshot (identifier, prefix, etc.).
-- Output: an allow decision with a repository-relative source, otherwise deny.
-- No external calls, credentials, writes or persistent state. This demonstrates
-- source selection and denial; replace it with application authorization in use.
function preflight(request)
    if request.identifier == "sample.png" then
        return { decision = "allow", source = "sample.png" }
    end
    return { decision = "deny" }
end
