#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sipi::preflight {
using HttpHeaders = std::map<std::string, std::vector<std::string>>;

/** Owning transport values. Headers are normalized to lower case by validation.
 * Body is binary-safe. Limits can only be lowered by the calling script. */
struct HttpRequest {
    std::string url;
    std::string method = "GET";
    HttpHeaders headers;
    std::optional<std::string> body;
    std::optional<std::string> ca_file;
    long timeout_ms = 5000;
    long connect_timeout_ms = 2000;
    std::size_t max_response_bytes = 1024 * 1024;
};
struct HttpResponse {
    long status = 0;
    HttpHeaders headers;
    HttpHeaders trailers;
    std::string body;
};
struct HttpFailure {
    const char* code;
    const char* message; // Static, credential-free text; never libcurl's error buffer.
};
struct HttpResult {
    HttpResponse response;
    std::optional<HttpFailure> failure;
};

/** Per-evaluation, lazily initialized HTTP session. Owns one libcurl easy handle,
 * reusing connections within a preflight call, never cookies or authentication.
 * Used by one thread at a time; independent sessions can run concurrently.
 * Four calls and ten seconds from the first call bound native network work.
 * Programming/configuration errors throw; transport failures are returned.
 * No request is allowed during script top-level initialization/validation. */
class HttpSession {
public:
    HttpSession();
    ~HttpSession();
    HttpSession(const HttpSession&) = delete;
    HttpSession& operator=(const HttpSession&) = delete;
    void enable() noexcept { enabled_ = true; }
    void disable() noexcept { enabled_ = false; }
    [[nodiscard]] HttpResult request(const HttpRequest& request);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool enabled_ = false;
    unsigned calls_ = 0;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
};
}
