#pragma once

#include "preflight/LuaPreflight.h"
#include "http/WorkerLimits.h"
#include "image/Bitonal.h"
#include "cache/ResultCache.h"
#include "cache/MetadataCache.h"
#include "image/SourceResolver.h"
#include "storage/S3ByteSource.h"
#include "iiifparser/IIIFGeometry.h"
#include <drogon/HttpResponse.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <map>
#include <thread>
#include <vector>

namespace sipi::http {

/** A bounded worker pool for IIIF policy, source resolution, metadata, geometry and JPEG/PNG delivery.
 * Owns the script snapshot; jobs own all request values and their completion.
 * Completion runs on a worker, using Drogon's thread-safe response callback.
 * stop() rejects new work, discards queued jobs and joins all active jobs. Call
 * it after app.run returns, before server resources disappear. No detached work.
 * Holds shared ownership of the optional result cache until worker destruction.
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
    /** Take ownership of policy/source settings and start the pool.
     * public_base_url must be validated configuration without a trailing slash;
     * it supplies information-document identities, never filesystem locations.
     * metadata_limits bounds one shared cache; either zero disables retention.
     * worker_count must be 1..32. Construction joins started threads if startup fails. */
    IiifWorker(std::optional<preflight::LuaPreflight> policy, SourceResolver sources,
               iiif::SizePolicy size_policy, std::string public_base_url,
               std::optional<S3Client> s3_client = std::nullopt,
               std::shared_ptr<cache::ResultCache> result_cache = nullptr,
               cache::MetadataCacheLimits metadata_limits = {},
               unsigned worker_count = default_worker_count(),
               BitonalMethod bitonal_method = BitonalMethod::threshold);
    ~IiifWorker();
    IiifWorker(const IiifWorker&) = delete;
    IiifWorker& operator=(const IiifWorker&) = delete;

    /** Queue at most 64 waiting requests; false means full or shutting down.
     * Callback is copied only on acceptance and must retain required HTTP state. */
    bool submit(Request request, const Callback& callback);
    /** Called by the owning thread, never from a completion; repeated calls are safe. */
    void stop();

private:
    struct Job { Request request; Callback callback; };
    void run();
    drogon::HttpResponsePtr resolve(const Request& request);

    std::optional<preflight::LuaPreflight> policy_;
    SourceResolver sources_;
    std::optional<S3Client> s3_client_;
    iiif::SizePolicy size_policy_;
    std::string public_base_url_;
    BitonalMethod bitonal_method_; // Immutable rendering policy shared by workers.
    std::shared_ptr<cache::ResultCache> result_cache_;
    cache::MetadataCache metadata_cache_; // Protected by metadata_mutex_; no I/O under this lock.
    std::mutex metadata_mutex_;
    // Only live fills/waiters are retained. Full serialized identities avoid hash collisions.
    std::mutex fills_mutex_;
    std::map<std::string, std::weak_ptr<std::mutex>> fills_;
    std::shared_ptr<std::mutex> fill_mutex(const cache::CacheKey& key);
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Job> jobs_;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};

} // namespace sipi::http
