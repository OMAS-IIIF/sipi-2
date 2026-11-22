#include "http/IiifWorker.h"
#include <atomic>
#include <barrier>
#include <utility>
#include <sstream>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>
#include <fstream>
#include <future>
#include <iostream>

namespace sipi::cache {
// Only this executable compiles the cache with the body-I/O failure seam.
thread_local std::function<void()> body_write_hook;
void before_result_cache_body_write() {
    if (auto hook = std::exchange(body_write_hook, {})) hook();
}
}

/** Admit four different fills before failing their unlocked body I/O together.
 * All images must still succeed, and the operator must receive just one error. */
bool concurrent_cache_failures(const std::filesystem::path& fixtures,
                               const std::filesystem::path& directory) {
    const auto cache_path = directory / "concurrent-failure-cache";
    std::filesystem::remove_all(cache_path); // Test-owned only.
    auto cache = std::make_shared<sipi::cache::ResultCache>(cache_path);
    std::barrier admitted(4);
    std::ostringstream output;
    auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(output);
    auto logger = spdlog::default_logger();
    logger->sinks().push_back(sink);
    struct RemoveSink {
        std::shared_ptr<spdlog::logger> logger;
        std::shared_ptr<spdlog::sinks::sink> sink;
        ~RemoveSink() { std::erase(logger->sinks(), sink); }
    } remove{logger, sink};
    sipi::http::IiifWorker worker{std::nullopt, sipi::SourceResolver{fixtures}, {},
        "http://localhost", std::nullopt, cache, {}, 4};
    std::vector<std::future<drogon::HttpResponsePtr>> results;
    for (unsigned size = 10; size < 14; ++size) {
        const auto parsed = sipi::iiif::parse_path("/tiled.tif/full/" + std::to_string(size) + ",/0/default.png");
        sipi::http::IiifWorker::Request request{std::get<sipi::iiif::ImageRequest>(parsed), {}, {}};
        request.client_connected = [&] {
            sipi::cache::body_write_hook = [&] {
                admitted.arrive_and_wait();
                throw sipi::cache::CacheError("Injected simultaneous body write failure");
            };
            return true;
        };
        auto done = std::make_shared<std::promise<drogon::HttpResponsePtr>>();
        results.push_back(done->get_future());
        if (!worker.submit(std::move(request), [done](const auto& result) { done->set_value(result); }))
            throw std::runtime_error("Unexpected queue rejection");
    }
    bool ok = true;
    for (auto& result : results) ok = result.get()->statusCode() == drogon::k200OK && ok;
    worker.stop();
    const auto log = output.str();
    const std::string marker = "IIIF result cache publication failed";
    const auto first = log.find(marker);
    return ok && first != std::string::npos && log.find(marker, first + marker.size()) == std::string::npos &&
        log.find("Injected simultaneous body write failure") != std::string::npos &&
        log.find("writes stay disabled until restart") != std::string::npos;
}

/** Exercise actual concurrent rendering without an HTTP event loop, so race
 * diagnostics can cover our pool/cache boundary independently of network code. */
bool rendering(const std::filesystem::path& fixtures, const std::filesystem::path& directory) {
    using Worker = sipi::http::IiifWorker;
    std::vector<Worker::Request> requests;
    for (const auto* name : {"tiled.tif", "alpha.tif", "source.jp2", "source.j2k", "source.jph", "source.jhc"})
        for (const auto* angle : {"0", "!90"}) {
            const auto parsed = sipi::iiif::parse_path(std::string("/") + name + "/full/max/" + angle + "/default.png");
            requests.push_back({std::get<sipi::iiif::ImageRequest>(parsed), {}, {}});
        }
    const auto submit = [](Worker& worker, const Worker::Request& request) {
        auto promise = std::make_shared<std::promise<drogon::HttpResponsePtr>>();
        auto future = promise->get_future();
        if (!worker.submit(request, [promise](const auto& result) { promise->set_value(result); }))
            throw std::runtime_error("Unexpected queue rejection");
        return future;
    };
    std::vector<std::string> expected;
    {
        Worker serial{std::nullopt, sipi::SourceResolver{fixtures}, {}, "http://localhost",
            std::nullopt, nullptr, {}, 1};
        for (const auto& request : requests) {
            auto result = submit(serial, request).get();
            if (result->statusCode() != drogon::k200OK) return false;
            expected.emplace_back(result->body());
        }
    }
    const auto cache_dir = directory / "parallel-cache";
    std::filesystem::remove_all(cache_dir); // Test-owned directory only.
    auto cache = std::make_shared<sipi::cache::ResultCache>(cache_dir);
    for (bool cached : {false, true}) {
        Worker parallel{std::nullopt, sipi::SourceResolver{fixtures}, {}, "http://localhost",
            std::nullopt, cached ? cache : nullptr, {}, 4};
        std::vector<std::future<drogon::HttpResponsePtr>> futures;
        for (unsigned repeat = 0; repeat < 4; ++repeat)
            for (const auto& request : requests) futures.push_back(submit(parallel, request));
        for (std::size_t i = 0; i < futures.size(); ++i) {
            const auto result = futures[i].get();
            if (result->statusCode() != drogon::k200OK || result->body() != expected[i % expected.size()]) return false;
        }
    }
    return true;
}

/** A configured queue limit replaces the default of 64 waiting requests. One
 * held completion occupies the single worker deterministically, without sleeps. */
bool configured_queue(const std::filesystem::path& deny) {
    sipi::http::IiifWorker worker{sipi::preflight::LuaPreflight(deny), sipi::SourceResolver{}, {},
        "http://127.0.0.1:8080", std::nullopt, nullptr, {}, 1, sipi::BitonalMethod::threshold,
        {}, {}, {}, 2};
    const sipi::http::IiifWorker::Request request{sipi::iiif::InfoRequest{{{}, "example"}}, {}, {}};
    std::promise<void> entered, release, drained;
    auto gate = release.get_future().share();
    std::atomic<int> completed = 0;
    bool ok = worker.submit(request, [&](const auto&) { entered.set_value(); gate.wait(); });
    entered.get_future().wait();
    for (int i = 0; i < 2; ++i)
        ok = worker.submit(request, [&](const auto&) { if (++completed == 2) drained.set_value(); }) && ok;
    ok = !worker.submit(request, [](const auto&) {}) && ok; // Third waiting request: 503.
    release.set_value();
    if (ok) drained.get_future().wait();
    return ok && completed == 2;
}

/** A disconnected client's work is skipped: before the policy runs (a deny
 * policy would otherwise answer 403) and again before rendering on a miss. */
bool disconnected_clients(const std::filesystem::path& deny, const std::filesystem::path& fixtures) {
    const auto run = [](sipi::http::IiifWorker& worker, sipi::http::IiifWorker::Request request) {
        std::promise<drogon::HttpResponsePtr> done;
        if (!worker.submit(std::move(request), [&](const auto& result) { done.set_value(result); }))
            throw std::runtime_error("Unexpected queue rejection");
        return done.get_future().get();
    };
    const auto skipped = [](const drogon::HttpResponsePtr& result) {
        return result->statusCode() == drogon::k503ServiceUnavailable &&
               result->body().find("client_disconnected") != std::string_view::npos &&
               result->getHeader("retry-after") == "1";
    };
    sipi::http::IiifWorker denying{sipi::preflight::LuaPreflight(deny), sipi::SourceResolver{}, {},
        "http://127.0.0.1:8080"};
    sipi::http::IiifWorker::Request queued{sipi::iiif::InfoRequest{{{}, "example"}}, {}, {}};
    queued.client_connected = [] { return false; };
    bool ok = skipped(run(denying, queued));
    queued.client_connected = [] { return true; };
    ok = run(denying, queued)->statusCode() == drogon::k403Forbidden && ok;

    sipi::http::IiifWorker rendering{std::nullopt, sipi::SourceResolver{fixtures}, {}, "http://localhost"};
    const auto parsed = sipi::iiif::parse_path("/tiled.tif/full/max/0/default.png");
    sipi::http::IiifWorker::Request image{std::get<sipi::iiif::ImageRequest>(parsed), {}, {}};
    auto probes = std::make_shared<std::atomic<int>>(0);
    // Connected when dequeued, gone by the time decoding would start.
    image.client_connected = [probes] { return ++*probes == 1; };
    ok = skipped(run(rendering, image)) && *probes == 2 && ok;
    image.client_connected = [] { return true; };
    ok = run(rendering, image)->statusCode() == drogon::k200OK && ok;
    image.client_connected = {}; // No probe: treated as connected.
    return run(rendering, image)->statusCode() == drogon::k200OK && ok;
}

/** Hold one completion to deterministically fill the queue without sleeps.
 * Verify backpressure, subsequent delivery, and rejection after shutdown. */
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    const auto path = directory / "deny.lua";
    { std::ofstream file(path); file << "function preflight() return {decision='deny'} end"; }
    sipi::http::IiifWorker worker{sipi::preflight::LuaPreflight(path), sipi::SourceResolver{}, {}, "http://127.0.0.1:8080", std::nullopt, nullptr, {}, 4};
    std::promise<void> entered, release, drained;
    auto gate = release.get_future().share();
    std::atomic<int> active = 0;
    std::atomic<int> completed = 0;
    std::atomic<bool> statuses_ok = true;
    const sipi::http::IiifWorker::Request request{sipi::iiif::InfoRequest{{{}, "example"}}, {}, {}};
    bool ok = true;
    // All four callbacks must enter before release: a single worker would deadlock.
    for (unsigned i = 0; i < 4; ++i) {
        ok = worker.submit(request, [&](const auto& response) {
            if (response->statusCode() != drogon::k403Forbidden) statuses_ok = false;
            if (++active == 4) entered.set_value();
            gate.wait();
        }) && ok;
    }
    entered.get_future().wait();
    for (int i = 0; i < 64; ++i) {
        ok = worker.submit(request, [&](const auto& response) {
            if (response->statusCode() != drogon::k403Forbidden) statuses_ok = false;
            if (++completed == 64) drained.set_value();
        }) && ok;
    }
    ok = !worker.submit(request, [](const auto&) {}) && ok;
    release.set_value();
    if (ok) drained.get_future().wait();
    worker.stop();
    worker.stop();
    ok = !worker.submit(request, [](const auto&) {}) && ok;
    ok = ok && statuses_ok && completed == 64;
    sipi::http::IiifWorker unconfigured{std::nullopt, sipi::SourceResolver{}, {}, "http://127.0.0.1:8080"};
    std::promise<drogon::HttpResponsePtr> missing_root;
    ok = unconfigured.submit(request, [&](const auto& response) { missing_root.set_value(response); }) && ok;
    const auto response = missing_root.get_future().get();
    ok = ok && response->statusCode() == drogon::k500InternalServerError &&
         response->body().find("source_not_configured") != std::string_view::npos;
    unconfigured.stop();
    ok = ok && sipi::http::default_worker_count(0) == 1 &&
        sipi::http::default_worker_count(2) == 2 && sipi::http::default_worker_count(128) == 4;
    for (unsigned invalid : {0u,33u}) {
        try {
            sipi::http::IiifWorker bad{std::nullopt, sipi::SourceResolver{}, {},
                "http://127.0.0.1:8080", std::nullopt, nullptr, {}, invalid};
            ok = false;
        } catch (const std::invalid_argument&) {}
    }
    const auto rejects = [](std::size_t queue, int webp) {
        try {
            sipi::http::IiifWorker bad{std::nullopt, sipi::SourceResolver{}, {}, "http://127.0.0.1:8080",
                std::nullopt, nullptr, {}, 1, sipi::BitonalMethod::threshold, {}, {}, {}, queue, webp};
            return false;
        } catch (const std::invalid_argument&) { return true; }
    };
    ok = rejects(0, 85) && rejects(1025, 85) && rejects(64, 0) && rejects(64, 101) && ok;
    ok = configured_queue(path) && ok;
    ok = disconnected_clients(path, argv[2]) && ok;
    ok = rendering(argv[2], directory) && ok;
    ok = concurrent_cache_failures(argv[2], directory) && ok;
    std::cout << (ok ? "Bounded worker checks passed\n" : "Worker check failed\n");
    return ok ? 0 : 1;
}
