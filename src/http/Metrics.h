#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sipi::http {

/** Lock-free operational counters for one IIIF worker pool, rendered in the
 * Prometheus text exposition format (version 0.0.4).
 *
 * All recording methods are noexcept, use relaxed atomics and may be called
 * concurrently from any thread. A rendered snapshot is not atomic across
 * metrics, which Prometheus tolerates. No labels carry request data (no
 * identifiers, paths or credentials), so the label sets are fixed and small. */
class Metrics {
public:
    using Duration = std::chrono::steady_clock::duration;

    enum class RequestKind { image, info, base };
    enum class PreflightOutcome { allow, deny, error };
    enum class CacheLookup { hit, miss, error };
    enum class CacheStore { stored, declined, error };

    /** Instantaneous pool state, read by the owner while rendering. */
    struct PoolState {
        unsigned workers = 0;
        unsigned active = 0;
        std::size_t queued = 0;
        std::size_t queue_limit = 0;
    };

    /** A worker completed a request with this HTTP status. */
    void response(int status) noexcept;
    /** A request was refused because the queue was full (503 iiif_busy). */
    void queue_rejected() noexcept;
    /** Work for a client that had already disconnected was skipped. */
    void client_disconnected() noexcept;
    /** One Lua preflight evaluation and its wall-clock duration. */
    void preflight(PreflightOutcome outcome, Duration elapsed) noexcept;
    void result_cache_lookup(CacheLookup outcome) noexcept;
    void result_cache_store(CacheStore outcome) noexcept;
    /** Worker time for one request, from dequeue to response, by kind. */
    void processing(RequestKind kind, Duration elapsed) noexcept;

    /** Prometheus text, including the pool state and build version. */
    [[nodiscard]] std::string render(const PoolState& pool, std::string_view version) const;

private:
    /** Fixed upper bounds in seconds; +Inf is implicit. Counts per bucket are
     * stored non-cumulatively and accumulated while rendering. */
    static constexpr std::array<double, 12> bucket_bounds{
        0.001, 0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1, 2.5, 5, 10};
    struct Histogram {
        std::array<std::atomic<std::uint64_t>, bucket_bounds.size() + 1> buckets{};
        std::atomic<std::uint64_t> sum_nanoseconds{0};
        void observe(Duration elapsed) noexcept;
        void render(std::string& out, std::string_view name, std::string_view labels) const;
    };
    /** HTTP status codes the worker produces; anything else counts as "other". */
    static constexpr std::array<int, 11> statuses{200, 303, 400, 403, 404, 406, 414, 500, 501, 503, 0};

    std::array<std::atomic<std::uint64_t>, statuses.size()> responses_{};
    std::atomic<std::uint64_t> queue_rejections_{0};
    std::atomic<std::uint64_t> disconnected_{0};
    std::array<std::atomic<std::uint64_t>, 3> preflight_outcomes_{};
    Histogram preflight_duration_;
    std::array<std::atomic<std::uint64_t>, 3> cache_lookups_{};
    std::array<std::atomic<std::uint64_t>, 3> cache_stores_{};
    std::array<Histogram, 3> processing_; // Indexed by RequestKind.
};

} // namespace sipi::http
