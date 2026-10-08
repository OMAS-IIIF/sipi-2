#include "preflight/HttpClient.h"

#include <curl/curl.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string_view>

namespace sipi::preflight {
namespace {
constexpr std::size_t body_limit = 1024 * 1024;
constexpr std::size_t header_limit = 64 * 1024;
constexpr std::size_t header_count_limit = 128;

void require(bool valid, const char* reason) {
    if (!valid) throw std::invalid_argument(reason);
}
bool token(std::string_view text) {
    if (text.empty()) return false;
    for (unsigned char c : text)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos)) return false;
    return true;
}
bool field_value(std::string_view text) {
    for (unsigned char c : text) if ((c < 32 && c != '\t') || c == 127) return false;
    return true;
}
std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& c : result) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return result;
}
std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}

/** Own our libcurl initialization reference independently of the AWS SDK's.
 * Function-static initialization is synchronized; all sessions die before teardown. */
struct CurlRuntime {
    CurlRuntime() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) throw std::runtime_error("HTTP initialization failed");
    }
    ~CurlRuntime() { curl_global_cleanup(); }
};
void initialize() {
    static CurlRuntime runtime;
    const auto* version = curl_version_info(CURLVERSION_NOW);
    require((version->features & CURL_VERSION_ASYNCHDNS) != 0,
            "HTTP requires libcurl with asynchronous DNS for bounded timeouts");
}
using Url = std::unique_ptr<CURLU, decltype(&curl_url_cleanup)>;
using CurlString = std::unique_ptr<char, decltype(&curl_free)>;

void validate(const HttpRequest& request) {
    require(request.url.size() <= 8192 && !request.url.empty(), "HTTP URL must contain 1..8192 bytes");
    for (unsigned char c : request.url) require(c > 32 && c != 127, "HTTP URL contains control characters or spaces");
    require(request.url.starts_with("http://") || request.url.starts_with("https://"), "HTTP URL must use http:// or https://");
    Url url(curl_url(), curl_url_cleanup);
    if (!url) throw std::bad_alloc();
    require(curl_url_set(url.get(), CURLUPART_URL, request.url.c_str(), 0) == CURLUE_OK, "Invalid HTTP URL");
    for (auto part : {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_FRAGMENT}) {
        char* text = nullptr;
        const auto code = curl_url_get(url.get(), part, &text, 0);
        CurlString owned(text, curl_free);
        require(code != CURLUE_OK, "HTTP URL must not contain credentials or a fragment");
    }
    require(request.method == "GET" || request.method == "HEAD" || request.method == "POST" ||
            request.method == "PUT" || request.method == "PATCH" || request.method == "DELETE" ||
            request.method == "OPTIONS", "Unsupported HTTP method");
    require(!request.body || request.body->size() <= body_limit, "HTTP request body exceeds 1 MiB");
    require(request.method != "HEAD" || !request.body, "HEAD must not include a body");
    require(request.timeout_ms >= 1 && request.timeout_ms <= 10000, "HTTP timeout_ms must be 1..10000");
    require(request.connect_timeout_ms >= 1 && request.connect_timeout_ms <= 2000, "HTTP connect_timeout_ms must be 1..2000");
    require(request.max_response_bytes >= 1 && request.max_response_bytes <= body_limit, "HTTP max_response_bytes must be 1..1048576");
    if (request.ca_file) require(!request.ca_file->empty() && request.ca_file->size() <= 4096 &&
                               request.ca_file->find('\0') == std::string::npos, "Invalid HTTP ca_file");
    std::size_t bytes = 0, count = 0;
    for (const auto& [name, values] : request.headers) {
        require(token(name), "Invalid HTTP header name");
        const auto key = lower(name);
        require(key != "host" && key != "content-length" && key != "transfer-encoding" &&
                key != "connection" && key != "expect" && key != "upgrade" && key != "te" &&
                key != "trailer" && !key.starts_with("proxy-"), "HTTP transport header cannot be overridden");
        require(!values.empty(), "HTTP header value list must not be empty");
        for (const auto& value : values) {
            require(field_value(value), "Invalid HTTP header value");
            require(++count <= header_count_limit && name.size() + value.size() + 4 <= header_limit - bytes,
                    "HTTP request headers exceed limit");
            bytes += name.size() + value.size() + 4;
        }
    }
}

struct Transfer {
    HttpResult result;
    std::size_t body_max;
    std::size_t header_bytes = 0;
    std::size_t header_count = 0;
    bool header_complete = false;
    void fail(const char* code, const char* message) noexcept { result.failure = HttpFailure{code, message}; }
};

/** libcurl callbacks contain all C++ exceptions and never invoke Lua. */
std::size_t write_body(char* data, std::size_t size, std::size_t count, void* context) noexcept {
    auto& transfer = *static_cast<Transfer*>(context);
    try {
        if (size && count > std::numeric_limits<std::size_t>::max() / size) return 0;
        const auto bytes = size * count;
        if (bytes > transfer.body_max - transfer.result.response.body.size()) {
            transfer.fail("response_limit", "HTTP response body exceeds limit"); return 0;
        }
        transfer.result.response.body.append(data, bytes);
        return bytes;
    } catch (...) { transfer.fail("memory", "HTTP response allocation failed"); return 0; }
}
std::size_t write_header(char* data, std::size_t size, std::size_t count, void* context) noexcept {
    auto& transfer = *static_cast<Transfer*>(context);
    try {
        if (size && count > std::numeric_limits<std::size_t>::max() / size) return 0;
        const auto bytes = size * count;
        if (bytes > header_limit - transfer.header_bytes) {
            transfer.fail("response_limit", "HTTP response headers exceed limit"); return 0;
        }
        transfer.header_bytes += bytes;
        std::string_view line(data, bytes);
        if (line.ends_with("\r\n")) line.remove_suffix(2);
        else if (line.ends_with("\n")) line.remove_suffix(1);
        if (line.starts_with("HTTP/")) {
            transfer.result.response.headers.clear();
            transfer.result.response.trailers.clear();
            transfer.header_complete = false;
        } else if (line.empty()) transfer.header_complete = true;
        else {
            const auto colon = line.find(':');
            if (colon == std::string_view::npos || !token(line.substr(0, colon)) || !field_value(line.substr(colon + 1))) {
                transfer.fail("protocol", "Malformed HTTP response header"); return 0;
            }
            if (++transfer.header_count > header_count_limit) {
                transfer.fail("response_limit", "HTTP response header count exceeds limit"); return 0;
            }
            auto& headers = transfer.header_complete ? transfer.result.response.trailers : transfer.result.response.headers;
            headers[lower(line.substr(0, colon))].emplace_back(trim(line.substr(colon + 1)));
        }
        return bytes;
    } catch (...) { transfer.fail("memory", "HTTP response allocation failed"); return 0; }
}
HttpFailure failure(CURLcode code) {
    switch (code) {
    case CURLE_OPERATION_TIMEDOUT: return {"timeout", "HTTP request timed out"};
    case CURLE_PEER_FAILED_VERIFICATION: case CURLE_SSL_CONNECT_ERROR: case CURLE_SSL_CACERT_BADFILE:
        return {"tls", "HTTP TLS verification or handshake failed"};
    default: return {"network", "HTTP transport failed"};
    }
}
}

struct HttpSession::Impl {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle{curl_easy_init(), curl_easy_cleanup};
    Impl() { if (!handle) throw std::bad_alloc(); }
};
HttpSession::HttpSession() = default;
HttpSession::~HttpSession() = default;

HttpResult HttpSession::request(const HttpRequest& request) {
    require(enabled_, "HTTP requests are only available inside preflight(request)");
    initialize();
    validate(request);
    const auto now = std::chrono::steady_clock::now();
    if (!deadline_) deadline_ = now + std::chrono::seconds(10);
    if (calls_ >= 4 || now >= *deadline_) return {{}, HttpFailure{"budget", "Preflight HTTP budget exhausted"}};
    ++calls_;
    if (!impl_) impl_ = std::make_unique<Impl>();
    auto* handle = impl_->handle.get();
    curl_easy_reset(handle); // Retains connections, resets all request options.
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline_ - now).count();
    Transfer transfer{{}, request.max_response_bytes};
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);
    // Reset borrowed callback/body/header pointers before their owners disappear,
    // also if setting an option throws. Connection reuse survives easy_reset.
    struct ResetOptions {
        CURL* handle;
        ~ResetOptions() { curl_easy_reset(handle); }
    } reset{handle};
    const auto add_header = [&](const std::string& line) {
        auto* next = curl_slist_append(headers.get(), line.c_str());
        if (!next) throw std::bad_alloc();
        headers.release(); headers.reset(next);
    };
    add_header("Expect:");
    for (const auto& [name, values] : request.headers)
        for (const auto& value : values) add_header(value.empty() ? name + ";" : name + ": " + value);
    const auto set = [&](CURLoption option, auto value) {
        if (curl_easy_setopt(handle, option, value) != CURLE_OK) throw std::runtime_error("HTTP transport setup failed");
    };
    set(CURLOPT_URL, request.url.c_str());
    set(CURLOPT_PROTOCOLS_STR, "http,https");
    set(CURLOPT_FOLLOWLOCATION, 0L);
    set(CURLOPT_PROXY, ""); // No ambient proxy environment or proxy credentials.
    set(CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
    set(CURLOPT_NOSIGNAL, 1L);
    set(CURLOPT_SSL_VERIFYPEER, 1L);
    set(CURLOPT_SSL_VERIFYHOST, 2L);
    if (request.ca_file) set(CURLOPT_CAINFO, request.ca_file->c_str());
    set(CURLOPT_TIMEOUT_MS, std::min(request.timeout_ms, std::max(1L, static_cast<long>(remaining))));
    set(CURLOPT_CONNECTTIMEOUT_MS, std::min(request.connect_timeout_ms, request.timeout_ms));
    set(CURLOPT_HTTPHEADER, headers.get());
    set(CURLOPT_WRITEFUNCTION, write_body);
    set(CURLOPT_WRITEDATA, &transfer);
    set(CURLOPT_HEADERFUNCTION, write_header);
    set(CURLOPT_HEADERDATA, &transfer);
    // No automatic decompression: the bounded body is returned as received.
    if (request.body) {
        set(CURLOPT_POSTFIELDS, request.body->data());
        set(CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.body->size()));
    }
    if (request.method == "HEAD") set(CURLOPT_NOBODY, 1L);
    set(CURLOPT_CUSTOMREQUEST, request.method.c_str());
    const auto code = curl_easy_perform(handle);
    if (!transfer.result.failure && code != CURLE_OK) transfer.result.failure = failure(code);
    if (!transfer.result.failure) {
        if (curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &transfer.result.response.status) != CURLE_OK ||
            transfer.result.response.status < 200 || transfer.result.response.status > 599)
            transfer.fail("protocol", "Invalid final HTTP status");
    }
    return std::move(transfer.result);
}
}
