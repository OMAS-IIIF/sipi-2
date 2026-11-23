#pragma once

#include "preflight/LuaPreflight.h"
#include "http/WorkerLimits.h"
#include "image/Bitonal.h"
#include "imgformats/JpegOptions.h"
#include "imgformats/WebpWriter.h"
#include "cache/ResultCache.h"
#include "cache/MetadataCache.h"
#include "cache/MaskCache.h"
#include "cache/VariantCache.h"
#include "image/SourceResolver.h"
#include "storage/S3ByteSource.h"
#include "iiifparser/IIIFGeometry.h"
#include "http/Metrics.h"
#include <drogon/HttpResponse.h>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <deque>
#include <functional>
#include <mutex>
#include <map>
#include <thread>
#include <vector>

namespace sipi::http {

/** A bounded worker pool for IIIF policy, source resolution, metadata, geometry and JPEG/PNG/WebP delivery.
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
        std::optional<std::string> accept = std::nullopt;
        bool redirect_to_info = false; // Info authorization/admission, then base-URI 303.
        preflight::QueryParameters query_parameters{};
        preflight::RequestMethod method = preflight::RequestMethod::get;
        // Validated raw query, used only to preserve credentials on the base redirect.
        std::string redirect_query{};
        /** Optional liveness probe of the client connection, called on a worker
         * thread; the probe must be thread-safe. False skips policy evaluation and rendering for a client that
         * has already gone away; absent means "assume connected". */
        std::function<bool()> client_connected{};
    };
    /** Take ownership of policy/source settings and start the pool.
     * public_base_url must be validated configuration without a trailing slash;
     * it supplies information-document identities, never filesystem locations.
     * metadata_limits and mask_limits bound separate pool-wide caches; either
     * zero limit disables retention. Mask I/O/decode occurs outside its mutex;
     * concurrent cold misses may decode independently. variant_limits bounds
     * complete immutable public rasters; same-variant materialization is serialized
     * through the shared fill registry, without I/O under the LRU mutex.
     * size_policy is bounded by the buffered renderer's public defaults/ceilings;
     * the same policy is advertised and enforced, with source admission on full/max.
     * jpeg_options are validated and shared immutably by cache lookup and encoding.
     * worker_count must be 1..32 and queue_limit 1..1024 (waiting requests beyond
     * the active ones). webp_quality is 1..100 and, like jpeg_options, is part of
     * the cache identity. Construction joins started threads if startup fails. */
    IiifWorker(std::optional<preflight::LuaPreflight> policy, SourceResolver sources,
               iiif::SizePolicy size_policy, std::string public_base_url,
               std::optional<S3Client> s3_client = std::nullopt,
               std::shared_ptr<cache::ResultCache> result_cache = nullptr,
               cache::MetadataCacheLimits metadata_limits = {},
               unsigned worker_count = default_worker_count(),
               BitonalMethod bitonal_method = BitonalMethod::threshold,
               cache::MaskCacheLimits mask_limits = {},
               cache::VariantCacheLimits variant_limits = {},
               JpegOptions jpeg_options = {},
               std::size_t queue_limit = default_queue_limit,
               int webp_quality = default_webp_quality);
    ~IiifWorker();
    IiifWorker(const IiifWorker&) = delete;
    IiifWorker& operator=(const IiifWorker&) = delete;

    /** Queue at most queue_limit waiting requests; false means full or shutting down.
     * Callback is copied only on acceptance and must retain required HTTP state. */
    bool submit(Request request, const Callback& callback);
    /** Called by the owning thread, never from a completion; repeated calls are safe. */
    void stop();
    /** Prometheus text for this pool (counters, latencies, queue state).
     * Safe from any thread, including Drogon's I/O loop; never waits for a worker. */
    [[nodiscard]] std::string metrics_text(std::string_view version) const;

private:
    struct Job { Request request; Callback callback; };
    // Pipeline values; defined in IiifWorker.cpp next to the stages using them.
    struct Watermarking;
    struct PublicImage;
    struct Encoding;
    struct CacheFill;
    struct DecodedRegion;

    void run();
    /** One request through the pipeline; never throws. */
    drogon::HttpResponsePtr resolve(const Request& request);
    // Pipeline stages, in order. They throw on failure; see failure_response.
    drogon::HttpResponsePtr process(const Request& request);
    std::optional<preflight::Allow> authorize(const Request& request, const iiif::Resource& resource);
    std::unique_ptr<ByteSource> open_source(std::string_view identifier,
                                            const std::optional<SourceOverride>& selection) const;
    Watermarking load_watermark(std::string_view identifier, const preflight::Watermark& policy);
    cache::MetadataPtr load_metadata(const ByteSource& source);
    std::variant<PublicImage, drogon::HttpResponsePtr> admit(
        cache::MetadataPtr metadata, const std::optional<preflight::ResolutionLimit>& resolution) const;
    drogon::HttpResponsePtr info_response(const Request& request, const iiif::Resource& resource,
                                          const PublicImage& image) const;
    drogon::HttpResponsePtr deliver_image(const Request& request, const iiif::ImageRequest& image,
        const ByteSource& source, const PublicImage& canvas, const Watermarking& watermark,
        const std::optional<preflight::Watermark>& watermark_policy);
    CacheFill lookup_cached(const ByteSource& source, const PublicImage& canvas,
        const iiif::ImageGeometry& plan, const iiif::ImageRequest& image, const Encoding& encoding,
        const Watermarking& watermark, bool watermarked);
    DecodedRegion read_public_pixels(const ByteSource& source, const PublicImage& canvas,
                                     const iiif::ImageGeometry& plan);
    std::string render(const ByteSource& source, const PublicImage& canvas,
        const iiif::ImageGeometry& plan, const iiif::ImageRequest& image, const Encoding& encoding,
        const Watermarking& watermark, const std::optional<preflight::Watermark>& watermark_policy);
    void publish(CacheFill& fill, const cache::ResultMetadata& output, const std::string& encoded);
    static drogon::HttpResponsePtr failure_response(std::exception_ptr failure);

    std::optional<preflight::LuaPreflight> policy_;
    SourceResolver sources_;
    std::optional<S3Client> s3_client_;
    iiif::SizePolicy size_policy_;
    std::string public_base_url_;
    const JpegOptions jpeg_options_; // Same immutable settings for cache and encoder.
    const int webp_quality_; // Likewise shared by cache identity and encoder.
    const std::size_t queue_limit_;
    BitonalMethod bitonal_method_; // Immutable rendering policy shared by workers.
    std::shared_ptr<cache::ResultCache> result_cache_;
    std::once_flag cache_write_error_; // Concurrent in-flight failures log only once.
    cache::MetadataCache metadata_cache_; // Protected by metadata_mutex_; no I/O under this lock.
    std::mutex metadata_mutex_;
    cache::MaskCache mask_cache_; // Protected by mask_mutex_; decoding stays outside the lock.
    std::mutex mask_mutex_;
    cache::VariantCache variant_cache_; // Complete immutable reduced sources, before overlays.
    std::mutex variant_mutex_;
    // Only live fills/waiters are retained. Full serialized identities avoid hash collisions.
    std::mutex fills_mutex_;
    std::map<std::string, std::weak_ptr<std::mutex>> fills_;
    std::shared_ptr<std::mutex> fill_mutex(const cache::CacheKey& key);
    Metrics metrics_;
    std::atomic<unsigned> active_{0};
    mutable std::mutex mutex_; // Guards jobs_ and stopping_; also read by metrics_text.
    std::condition_variable ready_;
    std::deque<Job> jobs_;
    bool stopping_ = false;
    std::vector<std::thread> threads_;
};

} // namespace sipi::http
