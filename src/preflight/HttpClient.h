#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
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

/** Idle libcurl easy handles shared by all HTTP sessions of one policy.
 *
 * A libcurl handle keeps its connection cache and TLS session cache across
 * requests. Returning handles here after each preflight evaluation lets the
 * next evaluation reuse an open keep-alive connection to the same service
 * instead of paying a new TCP and TLS handshake on every image request.
 *
 * Only transport state survives: every request ends with curl_easy_reset(),
 * which clears all options, headers, bodies and credentials, and libcurl's
 * cookie engine is never enabled. libcurl reuses a connection only for a
 * matching scheme, host, port and TLS configuration (for example ca_file).
 *
 * Thread-safe. A handle is used by one session at a time; at most
 * max_idle handles are retained, extra ones are closed on release.
 * Destroy the pool before process exit (it must not outlive libcurl's global
 * state); LuaPreflight owns it for the lifetime of the policy. */
class HttpHandlePool {
public:
    static constexpr std::size_t max_idle = 32; // Matches the worker limit.
    HttpHandlePool();
    ~HttpHandlePool();
    HttpHandlePool(const HttpHandlePool&) = delete;
    HttpHandlePool& operator=(const HttpHandlePool&) = delete;
private:
    friend class HttpSession;
    struct Handle;
    std::unique_ptr<Handle> acquire();
    void release(std::unique_ptr<Handle> handle) noexcept;
    std::mutex mutex_;
    std::vector<std::unique_ptr<Handle>> idle_;
};

/** Per-evaluation HTTP session with the native budget of one preflight call.
 * Borrows a handle from the pool on its first request and returns it on
 * destruction, so connections are reused within and across evaluations;
 * cookies and authentication are never reused.
 * Used by one thread at a time; independent sessions can run concurrently.
 * Four calls and ten seconds from the first call bound native network work.
 * Programming/configuration errors throw; transport failures are returned.
 * No request is allowed during script top-level initialization/validation.
 * The pool must outlive the session. */
class HttpSession {
public:
    explicit HttpSession(HttpHandlePool& pool);
    ~HttpSession();
    HttpSession(const HttpSession&) = delete;
    HttpSession& operator=(const HttpSession&) = delete;
    void enable() noexcept { enabled_ = true; }
    void disable() noexcept { enabled_ = false; }
    [[nodiscard]] HttpResult request(const HttpRequest& request);
private:
    HttpHandlePool& pool_;
    std::unique_ptr<HttpHandlePool::Handle> handle_;
    bool enabled_ = false;
    unsigned calls_ = 0;
    std::optional<std::chrono::steady_clock::time_point> deadline_;
};
}
