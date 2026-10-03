-- Example IIIF policy. Configure preflight-script to enable it for HTTP requests.
-- Replace the explicit source below with an existing local image.
function preflight(request)
    if request.identifier == "example" then
        return {
            decision = "allow",
            source = "/srv/images/example.tif"
        }
    end

    return { decision = "deny" }
end
