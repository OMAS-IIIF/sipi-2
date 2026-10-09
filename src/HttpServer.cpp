#include "HttpServer.h"
#include <algorithm>
#include "http/IiifWorker.h"
#include "http/HttpProtocol.h"
#include "iiifparser/IIIFParser.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <spdlog/spdlog.h>

#include <functional>
#include <memory>
#include <utility>
#include <string>
#include <string_view>

namespace sipi {
namespace {

/** Build application errors as JSON; serialization also escapes message text. */
drogon::HttpResponsePtr json_error(drogon::HttpStatusCode status,
                                   std::string_view code, std::string_view message) {
    Json::Value body;
    body["error"] = std::string(code);
    body["message"] = std::string(message);
    auto response = drogon::HttpResponse::newHttpJsonResponse(body);
    response->setStatusCode(status);
    return response;
}

/** Current endpoints are read-only. Check methods before routing so a mismatch
 * cannot fall through to IIIF or reach Drogon's automatic OPTIONS/CORS response.
 * Drogon dispatches HEAD as GET and removes its body on transmission.
 * Revisit this policy when adding a write endpoint such as /upload.
 */
void read_only_methods(const drogon::HttpRequestPtr& request,
                       drogon::AdviceCallback&& callback,
                       drogon::AdviceChainCallback&& next) {
    if (request->method() == drogon::Get || request->method() == drogon::Head) {
        next();
        return;
    }
    if (request->method() == drogon::Options) {
        const auto method = request->getHeader("access-control-request-method");
        if (!method.empty() && !http::allowed_cors_preflight(method,
                request->getHeader("access-control-request-headers"))) {
            callback(json_error(drogon::k403Forbidden, "cors_preflight_denied",
                                "Requested cross-origin method or headers are not supported"));
            return;
        }
        auto response = drogon::HttpResponse::newHttpResponse();
        response->setStatusCode(drogon::k204NoContent);
        response->addHeader("Allow", "GET, HEAD, OPTIONS");
        response->addHeader("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS");
        response->addHeader("Access-Control-Allow-Headers", "Authorization, Accept, Content-Type");
        response->addHeader("Cache-Control", "no-store");
        callback(response);
        return;
    }
    auto response = json_error(drogon::k405MethodNotAllowed, "method_not_allowed",
                               "This resource accepts GET, HEAD and OPTIONS only");
    response->addHeader("Allow", "GET, HEAD, OPTIONS");
    callback(response);
}

/** Liveness only: confirms HTTP handling, not image storage or decoder readiness. */
void health(const drogon::HttpRequestPtr&,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    Json::Value body;
    body["status"] = "ok";
    body["message"] = "I'm alive";
    body["version"] = SIPI_VERSION;
    callback(drogon::HttpResponse::newHttpJsonResponse(body));
}

/** Stable JSON names for syntax error components; HTTP mapping stays here. */
std::string_view component_name(iiif::Component component) {
    switch (component) {
    case iiif::Component::path: return "path";
    case iiif::Component::region: return "region";
    case iiif::Component::size: return "size";
    case iiif::Component::rotation: return "rotation";
    case iiif::Component::quality_format: return "quality_format";
    }
    return "unknown";
}

/** Parse the original encoded path exactly once. Successful syntax currently
 * invokes optional preflight before source lookup and metadata/geometry handling.
 * Results stay local; only the response is retained for asynchronous sending.
 */
void iiif_request(const drogon::HttpRequestPtr& request,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                  const std::shared_ptr<http::IiifWorker>& worker) {
    const auto routed = http::route_iiif_path(request->getOriginalPath());
    const auto& result = routed.query;
    if (const auto* error = std::get_if<iiif::ParseError>(&result)) {
        Json::Value body;
        body["error"] = "invalid_iiif_syntax";
        body["component"] = std::string(component_name(error->component));
        body["message"] = error->message;
        auto response = drogon::HttpResponse::newHttpJsonResponse(body);
        response->setStatusCode(drogon::k400BadRequest);
        callback(response);
        return;
    }
    auto parameters = http::parse_query_parameters(request->query());
    if (const auto* error = std::get_if<http::QueryError>(&parameters)) {
        auto response = json_error(static_cast<drogon::HttpStatusCode>(
                                       *error == http::QueryError::limit ? 414 : 400),
                                   "invalid_query", "Invalid or oversized query parameters");
        response->addHeader("Cache-Control", "no-store");
        callback(response);
        return;
    }
    if (const auto* image = std::get_if<iiif::ImageRequest>(&result)) {
        spdlog::debug("Parsed IIIF image: identifier='{}', prefix_segments={}, "
                      "rotation={}, mirrored={}, allow_upscaling={}",
                      image->resource.identifier, image->resource.prefix.size(),
                      image->rotation.degrees, image->rotation.mirrored,
                      image->size.allow_upscaling);
    } else {
        const auto& info = std::get<iiif::InfoRequest>(result);
        spdlog::debug("Parsed IIIF info: identifier='{}', prefix_segments={}",
                      info.resource.identifier, info.resource.prefix.size());
    }
    const auto header = [&](const char* name) -> std::optional<std::string> {
        const auto& headers = request->headers();
        const auto found = headers.find(name);
        if (found == headers.end()) return std::nullopt;
        return found->second;
    };
    http::IiifWorker::ParsedRequest query = std::get_if<iiif::ImageRequest>(&result)
        ? http::IiifWorker::ParsedRequest{std::get<iiif::ImageRequest>(result)}
        : http::IiifWorker::ParsedRequest{std::get<iiif::InfoRequest>(result)};
    // Retain the HTTP request until completion, including after disconnect.
    http::IiifWorker::Callback complete = [request, callback](const auto& response) { callback(response); };
    http::IiifWorker::Request input{std::move(query), header("authorization"), header("cookie")};
    input.accept = header("accept");
    input.redirect_to_info = routed.redirect_to_info;
    input.query_parameters = std::get<http::QueryParameters>(std::move(parameters));
    // Drogon routes HEAD through GET but retains the original method in isHead().
    input.method = request->isHead()
        ? preflight::RequestMethod::head : preflight::RequestMethod::get;
    if (routed.redirect_to_info) input.redirect_query = request->query();
    for (const auto& [name, value] : request->cookies()) input.cookies.emplace(name, value);
    if (!worker->submit(std::move(input), complete)) {
        auto response = json_error(drogon::k503ServiceUnavailable, "iiif_busy",
                                   "Image request processing is temporarily busy");
        response->addHeader("Cache-Control", "no-store");
        response->addHeader("Retry-After", "1");
        callback(response);
    }
    return;
}

/** Escape untrusted request text for an HTML text node, not a URL or script. */
std::string escape_html(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
        case '&': escaped += "&amp;"; break;
        case '<': escaped += "&lt;"; break;
        case '>': escaped += "&gt;"; break;
        case '"': escaped += "&quot;"; break;
        case '\'': escaped += "&#39;"; break;
        default: escaped += character;
        }
    }
    return escaped;
}

/**
 * Return a small HTML response immediately. Drogon retains shared ownership of
 * the response for asynchronous transmission after the callback returns.
 * Reconstruct the backend URL from Host, original encoded path, and raw query;
 * proxy headers are intentionally not trusted without a proxy trust policy.
 */
void hello(const drogon::HttpRequestPtr& request,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    std::string message = "Hello World";
    if (request->path() != "/hello" && request->path() != "/hello/") {
        auto host = request->getHeader("host");
        if (host.empty()) {
            host = request->getLocalAddr().toIpPort();
        }
        std::string url = "http://" + host + request->getOriginalPath();
        if (!request->query().empty()) {
            url += "?" + request->query();
        }
        message = "You called me with the URL " + escape_html(url);
    }
    auto response = drogon::HttpResponse::newHttpResponse();
    response->setContentTypeCode(drogon::CT_TEXT_HTML);
    response->setBody("<!doctype html><html lang=\"en\"><head>"
                      "<meta charset=\"utf-8\"><title>SIPI-2</title>"
                      "</head><body><p>" + message + "</p></body></html>");
    callback(response);
}

} // namespace

void run_http_server(const ServerConfig& config, std::optional<preflight::LuaPreflight> policy,
                     std::shared_ptr<cache::ResultCache> result_cache) {
    SourceResolver sources(config.iiif_root, config.iiif_storage, config.s3_bucket); // Validate configured root before listening.
    iiif::SizePolicy size_policy;
    size_policy.allow_upscaling = config.size_upscaling;
    size_policy.max_width = config.output_limits.max_width;
    size_policy.max_height = config.output_limits.max_height
        ? config.output_limits.max_height : config.output_limits.max_width;
    size_policy.max_area = config.output_limits.max_area;
    const auto public_base_url = config.public_base_url.value_or("http://127.0.0.1:" + std::to_string(config.port));
    std::optional<S3Client> s3_client;
    if (config.s3_bucket) s3_client.emplace(config.s3);
    auto worker = std::make_shared<http::IiifWorker>(std::move(policy), std::move(sources), size_policy, public_base_url, std::move(s3_client), std::move(result_cache), config.metadata_cache_limits, config.iiif_workers, config.bitonal_method, config.mask_cache_limits, config.variant_cache_limits, config.jpeg);
    const auto port = config.port;
    auto& app = drogon::app();
    app.addListener(config.listen_address, port);
    app.setThreadNum(1);
    app.setFileTypes({});
    app.registerPreSendingAdvice([origins = config.cors_origins,
                                  credentials = config.cors_credentials](
            const drogon::HttpRequestPtr& request, const drogon::HttpResponsePtr& response) {
        // Apply to successes, errors and redirects, outside the encoded-image cache.
        const bool wildcard = origins.size() == 1 && origins.front() == "*";
        const auto& origin = request->getHeader("origin");
        if (!wildcard) {
            const auto vary = response->getHeader("vary");
            response->addHeader("Vary", vary.empty() ? "Origin" : vary + ", Origin");
        }
        if (wildcard || std::find(origins.begin(), origins.end(), origin) != origins.end()) {
            response->addHeader("Access-Control-Allow-Origin", wildcard ? "*" : origin);
            if (credentials) response->addHeader("Access-Control-Allow-Credentials", "true");
            response->addHeader("Access-Control-Expose-Headers", "Link, Location");
        }
    });
    app.registerPreRoutingAdvice(&read_only_methods);
    app.registerHandler("/health", &health, {drogon::Get});
    // Exact routes win; regex routes are tried in registration order.
    app.registerHandlerViaRegex("/hello(?:/.*)?", &hello, {drogon::Get});
    app.registerHandlerViaRegex("/.*", [worker](const drogon::HttpRequestPtr& request,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        iiif_request(request, std::move(callback), worker);
    }, {drogon::Get});
    app.registerBeginningAdvice([port, address = config.listen_address] {
        spdlog::info("SIPI-2 listening on {} port {}", address, port);
    });
    app.run();
    worker->stop();
}

} // namespace sipi
