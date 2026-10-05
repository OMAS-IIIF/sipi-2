#include "http/IiifWorker.h"
#include <atomic>
#include <fstream>
#include <future>
#include <iostream>

/** Exercise actual concurrent rendering without an HTTP event loop, so race
 * diagnostics can cover our pool/cache boundary independently of network code. */
bool rendering(const std::filesystem::path& fixtures, const std::filesystem::path& directory) {
    using Worker = sipi::http::IiifWorker;
    std::vector<Worker::Request> requests;
    for (const auto* name : {"tiled.tif", "alpha.tif", "source.jp2", "source.j2k"})
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
    ok = rendering(argv[2], directory) && ok;
    std::cout << (ok ? "Bounded worker checks passed\n" : "Worker check failed\n");
    return ok ? 0 : 1;
}
