#include "http/Metrics.h"

#include <cmath>
#include <iostream>
#include <map>
#include <regex>
#include <sstream>
#include <thread>
#include <vector>

namespace {
int checks = 0, failures = 0;
void check(bool value, const char* name) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}

/** Parse "name{labels} value" samples; every non-comment line must match the
 * Prometheus text format, and every sample needs a preceding TYPE line. */
std::map<std::string, double> parse(const std::string& text) {
    static const std::regex sample(R"(^([a-zA-Z_:][a-zA-Z0-9_:]*(\{[^}]*\})?) (-?[0-9.eE+-]+|\+Inf)$)");
    static const std::regex type(R"(^# TYPE ([a-zA-Z_:][a-zA-Z0-9_:]*) (counter|gauge|histogram)$)");
    std::map<std::string, double> values;
    std::vector<std::string> types;
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        std::smatch match;
        if (line.starts_with("# HELP ")) continue;
        if (std::regex_match(line, match, type)) { types.push_back(match[1]); continue; }
        const bool valid = std::regex_match(line, match, sample);
        check(valid, "well-formed sample line");
        if (!valid) { std::cerr << "  line: " << line << '\n'; continue; }
        const auto key = match[1].str();
        const auto base = key.substr(0, key.find('{'));
        bool typed = false;
        for (const auto& name : types) typed |= base == name || base.starts_with(name + "_");
        check(typed, "sample has a TYPE declaration");
        values[key] = std::stod(match[3]);
    }
    return values;
}
}

int main() {
    using sipi::http::Metrics;
    using namespace std::chrono_literals;
    Metrics metrics;
    {
        const auto empty = parse(metrics.render({4, 0, 0, 64}, "1.2.3"));
        check(empty.at("sipi_build_info{version=\"1.2.3\"}") == 1, "build info");
        check(empty.at("sipi_iiif_workers") == 4 && empty.at("sipi_iiif_queue_limit") == 64, "pool state");
        check(empty.at("sipi_iiif_responses_total{code=\"200\"}") == 0, "zero counters are exported");
        check(empty.at("sipi_preflight_duration_seconds_count") == 0, "empty histogram");
    }

    metrics.response(200);
    metrics.response(200);
    metrics.response(503);
    metrics.response(418); // Not a status the worker produces: counted as other.
    metrics.queue_rejected();
    metrics.client_disconnected();
    metrics.result_cache_lookup(Metrics::CacheLookup::hit);
    metrics.result_cache_lookup(Metrics::CacheLookup::miss);
    metrics.result_cache_store(Metrics::CacheStore::declined);
    metrics.preflight(Metrics::PreflightOutcome::allow, 3ms);
    metrics.preflight(Metrics::PreflightOutcome::deny, 200ms);
    metrics.preflight(Metrics::PreflightOutcome::error, 20s); // Beyond the last bucket.
    metrics.processing(Metrics::RequestKind::image, 1ms);    // Exactly on a bound: le is inclusive.

    const auto values = parse(metrics.render({4, 2, 7, 64}, "1.2.3"));
    check(values.at("sipi_iiif_responses_total{code=\"200\"}") == 2, "200 responses");
    check(values.at("sipi_iiif_responses_total{code=\"503\"}") == 1, "503 responses");
    check(values.at("sipi_iiif_responses_total{code=\"other\"}") == 1, "unlisted status");
    check(values.at("sipi_iiif_active_workers") == 2 && values.at("sipi_iiif_queue_length") == 7, "gauges");
    check(values.at("sipi_iiif_queue_rejections_total") == 1, "queue rejection");
    check(values.at("sipi_iiif_disconnected_skips_total") == 1, "disconnect skip");
    check(values.at("sipi_result_cache_lookups_total{outcome=\"hit\"}") == 1 &&
          values.at("sipi_result_cache_lookups_total{outcome=\"miss\"}") == 1, "cache lookups");
    check(values.at("sipi_result_cache_stores_total{outcome=\"declined\"}") == 1, "cache stores");
    check(values.at("sipi_preflight_evaluations_total{outcome=\"deny\"}") == 1, "preflight outcome");
    // Histogram buckets are cumulative and end with +Inf == count.
    check(values.at("sipi_preflight_duration_seconds_bucket{le=\"0.001\"}") == 0, "below first observation");
    check(values.at("sipi_preflight_duration_seconds_bucket{le=\"0.005\"}") == 1, "3 ms bucket");
    check(values.at("sipi_preflight_duration_seconds_bucket{le=\"0.25\"}") == 2, "200 ms bucket");
    check(values.at("sipi_preflight_duration_seconds_bucket{le=\"10\"}") == 2, "20 s exceeds last bound");
    check(values.at("sipi_preflight_duration_seconds_bucket{le=\"+Inf\"}") == 3, "+Inf holds all");
    check(values.at("sipi_preflight_duration_seconds_count") == 3, "histogram count");
    check(std::abs(values.at("sipi_preflight_duration_seconds_sum") - 20.203) < 1e-9, "histogram sum");
    check(values.at("sipi_iiif_processing_seconds_bucket{kind=\"image\",le=\"0.001\"}") == 1, "inclusive bound");
    check(values.at("sipi_iiif_processing_seconds_count{kind=\"info\"}") == 0, "per-kind histograms");

    // Concurrent recording loses no increments.
    Metrics shared;
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) threads.emplace_back([&] {
        for (int i = 0; i < 10000; ++i) {
            shared.response(200);
            shared.processing(Metrics::RequestKind::info, 2ms);
        }
    });
    // Read while writers are active: +Inf and count must share one snapshot.
    for (int scrape = 0; scrape < 100; ++scrape) {
        const auto snapshot = parse(shared.render({1, 0, 0, 1}, "x"));
        check(snapshot.at("sipi_iiif_processing_seconds_bucket{kind=\"info\",le=\"+Inf\"}") ==
              snapshot.at("sipi_iiif_processing_seconds_count{kind=\"info\"}"),
              "concurrent histogram count equals +Inf");
    }
    for (auto& thread : threads) thread.join();
    const auto totals = parse(shared.render({1, 0, 0, 1}, "x"));
    check(totals.at("sipi_iiif_responses_total{code=\"200\"}") == 80000, "concurrent counter");
    check(totals.at("sipi_iiif_processing_seconds_count{kind=\"info\"}") == 80000, "concurrent histogram");

    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
