#pragma once

#include "preflight/LuaPreflight.h"
#include "image/SourceResolver.h"
#include "storage/S3ByteSource.h"
#include "iiifparser/IIIFGeometry.h"
#include <drogon/HttpResponse.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace sipi::http {

/** One bounded worker for IIIF policy, source resolution, metadata, geometry and JPEG delivery.
 * Owns the script snapshot; jobs own all request values and their completion.
 * Completion runs on the worker, using Drogon's thread-safe response callback.
 * stop() rejects new work, discards queued jobs and joins the current job. Call
 * it after app.run returns, before server resources disappear. No detached work.
 */
class IiifWorker {
public:
    using Callback = std::function<void(const drogon::HttpResponsePtr&)>;
    using ParsedRequest = std::variant<iiif::ImageRequest, iiif::InfoRequest>;
    /** Owned HTTP inputs; the query is the sole source of resource/geometry data. */
    struct Request {
        ParsedRequest query;
        std::optional<std::string> authorization;
        std::optional<std::string> cookie;
        std::map<std::string, std::string> cookies{};
    };
    /** Take ownership of policy/source settings and start the worker.
     * public_base_url must be validated configuration without a trailing slash;
     * it supplies information-document identities, never filesystem locations. */
    IiifWorker(std::optional<preflight::LuaPreflight> policy, SourceResolver sources,
               iiif::SizePolicy size_policy, std::string public_base_url,
               std::optional<S3Client> s3_client = std::nullopt);
    ~IiifWorker();
    IiifWorker(const IiifWorker&) = delete;
    IiifWorker& operator=(const IiifWorker&) = delete;

    /** Queue at most 64 waiting requests; false means full or shutting down.
     * Callback is copied only on acceptance and must retain required HTTP state. */
    bool submit(Request request, const Callback& callback);
    void stop();

private:
    struct Job { Request request; Callback callback; };
    void run();
    drogon::HttpResponsePtr resolve(const Request& request) const;

    std::optional<preflight::LuaPreflight> policy_;
    SourceResolver sources_;
    std::optional<S3Client> s3_client_;
    iiif::SizePolicy size_policy_;
    std::string public_base_url_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> jobs_;
    bool stopping_ = false;
    std::thread thread_;
};

} // namespace sipi::http
