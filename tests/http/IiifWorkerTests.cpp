#include "http/IiifWorker.h"
#include <atomic>
#include <fstream>
#include <future>
#include <iostream>

/** Hold one completion to deterministically fill the queue without sleeps.
 * Verify backpressure, subsequent delivery, and rejection after shutdown. */
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    const auto path = directory / "deny.lua";
    { std::ofstream file(path); file << "function preflight() return {decision='deny'} end"; }
    sipi::http::IiifWorker worker{sipi::preflight::LuaPreflight(path), sipi::SourceResolver{}, {}, "http://127.0.0.1:8080"};
    std::promise<void> entered, release, drained;
    auto gate = release.get_future().share();
    std::atomic<int> completed = 0;
    std::atomic<bool> statuses_ok = true;
    const sipi::http::IiifWorker::Request request{sipi::iiif::InfoRequest{{{}, "example"}}, {}, {}};
    bool ok = worker.submit(request, [&](const auto& response) {
        statuses_ok = response->statusCode() == drogon::k403Forbidden;
        entered.set_value();
        gate.wait();
    });
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
    ok = !worker.submit(request, [](const auto&) {}) && ok;
    ok = ok && statuses_ok && completed == 64;
    sipi::http::IiifWorker unconfigured{std::nullopt, sipi::SourceResolver{}, {}, "http://127.0.0.1:8080"};
    std::promise<drogon::HttpResponsePtr> missing_root;
    ok = unconfigured.submit(request, [&](const auto& response) { missing_root.set_value(response); }) && ok;
    const auto response = missing_root.get_future().get();
    ok = ok && response->statusCode() == drogon::k500InternalServerError &&
         response->body().find("source_not_configured") != std::string_view::npos;
    unconfigured.stop();
    std::cout << (ok ? "Bounded worker checks passed\n" : "Worker check failed\n");
    return ok ? 0 : 1;
}
