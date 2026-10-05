#include "HttpServer.h"
#include "http/IiifWorker.h"
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
    auto response = json_error(drogon::k405MethodNotAllowed, "method_not_allowed",
                               "This resource accepts GET and HEAD only");
    response->addHeader("Allow", "GET, HEAD");
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
    const auto result = iiif::parse_path(request->getOriginalPath());
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
    const auto public_base_url = config.public_base_url.value_or("http://127.0.0.1:" + std::to_string(config.port));
    std::optional<S3Client> s3_client;
    if (config.s3_bucket) s3_client.emplace(config.s3);
    auto worker = std::make_shared<http::IiifWorker>(std::move(policy), std::move(sources), size_policy, public_base_url, std::move(s3_client), std::move(result_cache), config.metadata_cache_limits, config.iiif_workers, config.bitonal_method);
    const auto port = config.port;
    auto& app = drogon::app();
    app.addListener("127.0.0.1", port);
    app.setThreadNum(1);
    app.setFileTypes({});
    app.registerPreRoutingAdvice(&read_only_methods);
    app.registerHandler("/health", &health, {drogon::Get});
    // Exact routes win; regex routes are tried in registration order.
    app.registerHandlerViaRegex("/hello(?:/.*)?", &hello, {drogon::Get});
    app.registerHandlerViaRegex("/.*", [worker](const drogon::HttpRequestPtr& request,
        std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        iiif_request(request, std::move(callback), worker);
    }, {drogon::Get});
    app.registerBeginningAdvice([port] {
        spdlog::info("SIPI-2 listening on http://127.0.0.1:{}", port);
    });
    app.run();
    worker->stop();
}

} // namespace sipi
