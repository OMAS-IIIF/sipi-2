#include "http/Metrics.h"

#include <algorithm>
#include <charconv>

namespace sipi::http {
namespace {
constexpr auto relaxed = std::memory_order_relaxed;

void increment(std::atomic<std::uint64_t>& counter) noexcept { counter.fetch_add(1, relaxed); }

/** Shortest round-trip decimal; Prometheus accepts plain floats. */
std::string number(double value) {
    std::array<char, 32> text{};
    const auto result = std::to_chars(text.data(), text.data() + text.size(), value);
    return std::string(text.data(), result.ptr);
}

void header(std::string& out, std::string_view name, std::string_view type, std::string_view help) {
    out.append("# HELP ").append(name).append(" ").append(help).append("\n");
    out.append("# TYPE ").append(name).append(" ").append(type).append("\n");
}

void sample(std::string& out, std::string_view name, std::string_view labels, std::uint64_t value) {
    out.append(name);
    if (!labels.empty()) out.append("{").append(labels).append("}");
    out.append(" ").append(std::to_string(value)).append("\n");
}

constexpr std::array<std::string_view, 3> kinds{"image", "info", "base"};
} // namespace

void Metrics::Histogram::observe(Duration elapsed) noexcept {
    const auto nanoseconds = std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
    const double seconds = static_cast<double>(nanoseconds) / 1e9;
    const auto bucket = static_cast<std::size_t>(
        std::lower_bound(bucket_bounds.begin(), bucket_bounds.end(), seconds) - bucket_bounds.begin());
    increment(buckets[bucket]);
    sum_nanoseconds.fetch_add(static_cast<std::uint64_t>(nanoseconds), relaxed);
}

void Metrics::Histogram::render(std::string& out, std::string_view name, std::string_view labels) const {
    const std::string prefix = labels.empty() ? std::string{} : std::string(labels) + ",";
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < bucket_bounds.size(); ++i) {
        cumulative += buckets[i].load(relaxed);
        sample(out, std::string(name) + "_bucket", prefix + "le=\"" + number(bucket_bounds[i]) + "\"", cumulative);
    }
    cumulative += buckets.back().load(relaxed);
    sample(out, std::string(name) + "_bucket", prefix + "le=\"+Inf\"", cumulative);
    out.append(name).append("_sum");
    if (!labels.empty()) out.append("{").append(labels).append("}");
    out.append(" ").append(number(static_cast<double>(sum_nanoseconds.load(relaxed)) / 1e9)).append("\n");
    // The exposition contract requires _count == +Inf even during recording.
    sample(out, std::string(name) + "_count", labels, cumulative);
}

void Metrics::response(int status) noexcept {
    const auto known = std::find(statuses.begin(), statuses.end() - 1, status);
    increment(responses_[static_cast<std::size_t>(known - statuses.begin())]);
}
void Metrics::queue_rejected() noexcept { increment(queue_rejections_); }
void Metrics::client_disconnected() noexcept { increment(disconnected_); }
void Metrics::preflight(PreflightOutcome outcome, Duration elapsed) noexcept {
    increment(preflight_outcomes_[static_cast<std::size_t>(outcome)]);
    preflight_duration_.observe(elapsed);
}
void Metrics::result_cache_lookup(CacheLookup outcome) noexcept {
    increment(cache_lookups_[static_cast<std::size_t>(outcome)]);
}
void Metrics::result_cache_store(CacheStore outcome) noexcept {
    increment(cache_stores_[static_cast<std::size_t>(outcome)]);
}
void Metrics::processing(RequestKind kind, Duration elapsed) noexcept {
    processing_[static_cast<std::size_t>(kind)].observe(elapsed);
}

std::string Metrics::render(const PoolState& pool, std::string_view version) const {
    std::string out;
    out.reserve(8192);
    header(out, "sipi_build_info", "gauge", "SIPI-2 version; the value is always 1.");
    sample(out, "sipi_build_info", "version=\"" + std::string(version) + "\"", 1);

    header(out, "sipi_iiif_workers", "gauge", "Configured IIIF worker threads.");
    sample(out, "sipi_iiif_workers", "", pool.workers);
    header(out, "sipi_iiif_active_workers", "gauge", "Workers currently processing a request.");
    sample(out, "sipi_iiif_active_workers", "", pool.active);
    header(out, "sipi_iiif_queue_length", "gauge", "Requests waiting for a worker.");
    sample(out, "sipi_iiif_queue_length", "", pool.queued);
    header(out, "sipi_iiif_queue_limit", "gauge", "Configured maximum of waiting requests.");
    sample(out, "sipi_iiif_queue_limit", "", pool.queue_limit);

    header(out, "sipi_iiif_responses_total", "counter",
           "IIIF requests completed by a worker, by HTTP status (other: any unlisted status).");
    for (std::size_t i = 0; i < statuses.size(); ++i) {
        const auto code = statuses[i] ? std::to_string(statuses[i]) : std::string("other");
        sample(out, "sipi_iiif_responses_total", "code=\"" + code + "\"", responses_[i].load(relaxed));
    }
    header(out, "sipi_iiif_queue_rejections_total", "counter",
           "IIIF requests refused with 503 iiif_busy because the queue was full.");
    sample(out, "sipi_iiif_queue_rejections_total", "", queue_rejections_.load(relaxed));
    header(out, "sipi_iiif_disconnected_skips_total", "counter",
           "Queued or unrendered requests skipped because the client had disconnected.");
    sample(out, "sipi_iiif_disconnected_skips_total", "", disconnected_.load(relaxed));

    header(out, "sipi_iiif_processing_seconds", "histogram",
           "Worker time per request from dequeue to response, by request kind.");
    for (std::size_t i = 0; i < kinds.size(); ++i)
        processing_[i].render(out, "sipi_iiif_processing_seconds", "kind=\"" + std::string(kinds[i]) + "\"");

    header(out, "sipi_preflight_evaluations_total", "counter", "Lua preflight evaluations by outcome.");
    constexpr std::array<std::string_view, 3> decisions{"allow", "deny", "error"};
    for (std::size_t i = 0; i < decisions.size(); ++i)
        sample(out, "sipi_preflight_evaluations_total", "outcome=\"" + std::string(decisions[i]) + "\"",
               preflight_outcomes_[i].load(relaxed));
    header(out, "sipi_preflight_duration_seconds", "histogram",
           "Wall-clock time of one Lua preflight evaluation, including its HTTP calls.");
    preflight_duration_.render(out, "sipi_preflight_duration_seconds", "");

    header(out, "sipi_result_cache_lookups_total", "counter", "Persistent result-cache lookups by outcome.");
    constexpr std::array<std::string_view, 3> lookups{"hit", "miss", "error"};
    for (std::size_t i = 0; i < lookups.size(); ++i)
        sample(out, "sipi_result_cache_lookups_total", "outcome=\"" + std::string(lookups[i]) + "\"",
               cache_lookups_[i].load(relaxed));
    header(out, "sipi_result_cache_stores_total", "counter", "Persistent result-cache publications by outcome.");
    constexpr std::array<std::string_view, 3> stores{"stored", "declined", "error"};
    for (std::size_t i = 0; i < stores.size(); ++i)
        sample(out, "sipi_result_cache_stores_total", "outcome=\"" + std::string(stores[i]) + "\"",
               cache_stores_[i].load(relaxed));
    return out;
}

} // namespace sipi::http
