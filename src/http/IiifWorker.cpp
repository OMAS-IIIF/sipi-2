#include "http/IiifWorker.h"
#include "http/IiifInfo.h"
#include "http/ImageUri.h"
#include "http/DeliveryPolicy.h"
#include "http/HttpProtocol.h"
#include <spdlog/spdlog.h>
#include <utility>
#include "storage/LocalFileSource.h"
#include "image/AreaResize.h"
#include "image/AlphaComposite.h"
#include "image/ImageTransform.h"
#include "image/GrayConversion.h"
#include "image/Bitonal.h"
#include "image/Watermark.h"
#include "image/WatermarkColor.h"
#include <chrono>
#include <tuple>
#include <array>
#include <algorithm>
#include "imgformats/TiffReadPlan.h"
#include "imgformats/TiffMetadata.h"
#include "imgformats/TiffReader.h"
#include "imgformats/J2kReader.h"
#include "imgformats/J2kReadPlan.h"
#include <optional>
#include "imgformats/JpegWriter.h"
#include "imgformats/PngWriter.h"
#include "imgformats/WebpWriter.h"

namespace sipi::http {
namespace {
/** Probe/read one opened source on a metadata miss. Unknown formats return null;
 * reader errors propagate and must never become cached successes. */
cache::MetadataPtr read_source_metadata(const ByteSource& source) {
    // Identify TIFF, PNG, JPEG, JP2 or raw J2K from bytes, not the filename extension.
    // This does not decode pixels. The metadata reader validates the header.
    std::array<unsigned char, 12> magic{};
    const auto count = source.read_at(0, std::as_writable_bytes(std::span{magic}));
    const bool tiff = count >= 4 &&
        ((magic[0] == 'I' && magic[1] == 'I' && (magic[2] == 42 || magic[2] == 43) && magic[3] == 0) ||
         (magic[0] == 'M' && magic[1] == 'M' && magic[2] == 0 && (magic[3] == 42 || magic[3] == 43)));
    constexpr std::array<unsigned char, 12> jp2_magic{0,0,0,12,'j','P',' ', ' ',13,10,135,10};
    const bool j2k = (count >= 4 && magic[0] == 0xff && magic[1] == 0x4f &&
                         magic[2] == 0xff && magic[3] == 0x51) ||
                        (count == magic.size() && magic == jp2_magic);
    if (count >= 3 && magic[0] == 0xff && magic[1] == 0xd8 && magic[2] == 0xff)
        return std::make_shared<const cache::ImageMetadata>(read_jpeg_metadata(source));
    constexpr std::array<unsigned char, 8> png_magic{137,80,78,71,13,10,26,10};
    if (count >= png_magic.size() && std::equal(png_magic.begin(), png_magic.end(), magic.begin()))
        return std::make_shared<const cache::ImageMetadata>(read_png_metadata(source));
    if (tiff) return std::make_shared<const cache::ImageMetadata>(read_tiff_metadata(source));
    if (j2k) return std::make_shared<const cache::ImageMetadata>(read_j2k_metadata(source));
    return nullptr;
}
/** Read at the selected decoder resolution, retaining the exact crop window.
 * Used unchanged for original delivery and full-image public-variant generation. */
std::tuple<RasterImage8, PixelWindow, WatermarkGrid> read_pixels(
        const ByteSource& source, const cache::ImageMetadata& metadata,
        const iiif::ImageGeometry& plan) {
    const auto* tiff_metadata = std::get_if<TiffMetadata>(&metadata);
    const auto* j2k_metadata = std::get_if<J2kMetadata>(&metadata);
    const auto* jpeg_metadata = std::get_if<JpegMetadata>(&metadata);
    const auto* png_metadata = std::get_if<PngMetadata>(&metadata);

    if (tiff_metadata) {
        const auto read_plan = supported_tiff_plan(*tiff_metadata, plan);
        return {read_tiff_region(source, read_plan.directory, read_plan.read_region),
                {read_plan.horizontal, read_plan.vertical},
                {read_plan.directory.display_width(),read_plan.directory.display_height(),
                 read_plan.read_region.x,read_plan.read_region.y}};
    }
    if (png_metadata) {
        return {read_png_image(source, *png_metadata),
            {{plan.region.x, plan.region.width, 1}, {plan.region.y, plan.region.height, 1}},
            {png_metadata->width,png_metadata->height}};
    }
    if (jpeg_metadata) {
        // Decode only the requested rectangle (pixel-identical to a full
        // decode); the window and watermark grid are relative to it.
        auto region = read_jpeg_region(source, *jpeg_metadata, plan.region);
        return {std::move(region.image),
            {{0, plan.region.width, 1}, {0, plan.region.height, 1}},
            {jpeg_metadata->width, jpeg_metadata->height, region.x, region.y}};
    }
    const auto read_plan = plan_j2k_read(*j2k_metadata, plan);
    return {read_j2k_region(source, *j2k_metadata, read_plan), read_plan.window,
        {read_plan.level_width,read_plan.level_height,read_plan.reduced_region.x,read_plan.reduced_region.y}};
}

constexpr std::size_t encoded_limit = 64 * 1024 * 1024;

/** Build identical fresh/hit response headers; authorization is never cached. */
drogon::HttpResponsePtr image_response(std::string encoded, const std::string& media_type, const std::string& canonical) {
    auto result = drogon::HttpResponse::newHttpResponse();
    result->setContentTypeString(media_type);
    result->addHeader("Cache-Control", "no-store");
    result->addHeader("Link", "<" + canonical + ">;rel=\"canonical\"");
    result->setBody(std::move(encoded));
    return result;
}
/** Policy-dependent responses must not be reused by a browser/proxy cache.
 * Diagnostics deliberately contain neither credentials nor local source paths. */
drogon::HttpResponsePtr response(drogon::HttpStatusCode status,
                                const char* code, const char* message) {
    Json::Value body;
    body["error"] = code;
    body["message"] = message;
    auto result = drogon::HttpResponse::newHttpJsonResponse(body);
    result->setStatusCode(status);
    result->addHeader("Cache-Control", "no-store");
    return result;
}
/** The IIIF resource named by an image or info request; borrows from query. */
const iiif::Resource& resource_of(const IiifWorker::ParsedRequest& query) {
    if (const auto* image = std::get_if<iiif::ImageRequest>(&query)) return image->resource;
    return std::get<iiif::InfoRequest>(query).resource;
}
/** Transient refusal; Retry-After tells clients and proxies to try again. */
drogon::HttpResponsePtr retryable(const char* code, const char* message, unsigned seconds = 1) {
    auto result = response(drogon::k503ServiceUnavailable, code, message);
    result->addHeader("Retry-After", std::to_string(seconds));
    return result;
}
/** True when the HTTP layer reports that the client has already gone away.
 * Skipping its work frees the worker for live requests (for example when a
 * viewer cancels tile requests during fast zooming). No probe means connected. */
bool client_gone(const IiifWorker::Request& request) {
    return request.client_connected && !request.client_connected();
}
/** Request kind for latency metrics, matching the preflight request kinds. */
Metrics::RequestKind request_kind(const IiifWorker::Request& request) {
    if (request.redirect_to_info) return Metrics::RequestKind::base;
    return std::holds_alternative<iiif::ImageRequest>(request.query)
        ? Metrics::RequestKind::image : Metrics::RequestKind::info;
}
/** Nobody receives this response; it only completes Drogon's request cycle. */
drogon::HttpResponsePtr skipped_for_disconnected_client() {
    spdlog::debug("IIIF client disconnected; skipping remaining work");
    return retryable("client_disconnected", "The client connection was closed");
}
} // namespace

IiifWorker::IiifWorker(std::optional<preflight::LuaPreflight> policy, SourceResolver sources,
                       iiif::SizePolicy size_policy, std::string public_base_url, std::optional<S3Client> s3_client,
                       std::shared_ptr<cache::ResultCache> result_cache, cache::MetadataCacheLimits metadata_limits, unsigned worker_count, BitonalMethod bitonal_method, cache::MaskCacheLimits mask_limits, cache::VariantCacheLimits variant_limits, JpegOptions jpeg_options,
                       std::size_t queue_limit, int webp_quality)
    : policy_(std::move(policy)), sources_(std::move(sources)), s3_client_(std::move(s3_client)), size_policy_(bounded_output_policy(size_policy)),
      public_base_url_(std::move(public_base_url)), jpeg_options_(jpeg_options), webp_quality_(webp_quality),
      queue_limit_(queue_limit), bitonal_method_(bitonal_method), result_cache_(std::move(result_cache)),
      metadata_cache_(metadata_limits), mask_cache_(mask_limits), variant_cache_(variant_limits) {
    if (!jpeg_options_.valid())
        throw std::invalid_argument("Invalid JPEG encoding options");
    if (!worker_count || worker_count > maximum_worker_count)
        throw std::invalid_argument("IIIF worker count must be between 1 and 32");
    if (!queue_limit_ || queue_limit_ > maximum_queue_limit)
        throw std::invalid_argument("IIIF queue limit must be between 1 and 1024");
    if (webp_quality_ < 1 || webp_quality_ > 100)
        throw std::invalid_argument("WebP quality must be between 1 and 100");
    spdlog::info("IIIF processing pool: {} workers, queue limit {}", worker_count, queue_limit_);
    threads_.reserve(worker_count);
    try {
        for (unsigned i = 0; i < worker_count; ++i) threads_.emplace_back([this] { run(); });
    } catch (...) {
        stop();
        throw;
    }
}

/** Serialize lookup/render/publication for one result only. Expired entries are
 * pruned on acquisition, so bookkeeping is bounded by active worker requests.
 * Failed or declined publications allow the next waiter to retry normally. */
std::shared_ptr<std::mutex> IiifWorker::fill_mutex(const cache::CacheKey& key) {
    std::lock_guard lock(fills_mutex_);
    std::erase_if(fills_, [](const auto& entry) { return entry.second.expired(); });
    auto& entry = fills_[std::string(key.serialized())];
    auto result = entry.lock();
    if (!result) entry = result = std::make_shared<std::mutex>();
    return result;
}

IiifWorker::~IiifWorker() { stop(); }

bool IiifWorker::submit(Request request, const Callback& callback) {
    std::lock_guard lock(mutex_);
    if (stopping_) return false;
    if (jobs_.size() >= queue_limit_) {
        metrics_.queue_rejected();
        return false;
    }
    jobs_.push_back({std::move(request), callback});
    ready_.notify_one();
    return true;
}

void IiifWorker::stop() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        jobs_.clear();
    }
    ready_.notify_all();
    for (auto& thread : threads_) if (thread.joinable()) thread.join();
}

void IiifWorker::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, [this] { return stopping_ || !jobs_.empty(); });
            if (stopping_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        active_.fetch_add(1, std::memory_order_relaxed);
        const auto started = std::chrono::steady_clock::now();
        drogon::HttpResponsePtr result;
        // Queued work can outlive its client; check before any policy/storage work.
        if (client_gone(job.request)) {
            metrics_.client_disconnected();
            result = skipped_for_disconnected_client();
        } else {
            result = resolve(job.request);
            metrics_.processing(request_kind(job.request), std::chrono::steady_clock::now() - started);
        }
        metrics_.response(static_cast<int>(result->statusCode()));
        active_.fetch_sub(1, std::memory_order_relaxed);
        if (result->statusCode() == drogon::k200OK) {
            const auto existing = result->getHeader("link");
            result->addHeader("Link", existing.empty() ? std::string(level2_link)
                : existing + ", " + std::string(level2_link));
        }
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
        }
        try { job.callback(result); }
        catch (...) { spdlog::error("IIIF response callback failed"); }
    }
}

std::string IiifWorker::metrics_text(std::string_view version) const {
    Metrics::PoolState pool;
    pool.workers = static_cast<unsigned>(threads_.size());
    pool.active = active_.load(std::memory_order_relaxed);
    pool.queue_limit = queue_limit_;
    {
        std::lock_guard lock(mutex_);
        pool.queued = jobs_.size();
    }
    return metrics_.render(pool, version);
}

// ---------------------------------------------------------------------------
// Request pipeline. resolve() runs the stages below in this order:
//   authorize -> open_source -> load_watermark -> load_metadata -> admit
//   -> info_response | deliver_image (cache lookup -> render -> publish)
// Each stage either returns its result or throws; failure_response() maps any
// exception to the HTTP error contract in one place.
// ---------------------------------------------------------------------------

/** Required watermark: decoded mask plus the identity that enters the cache key. */
struct IiifWorker::Watermarking {
    cache::MaskPtr mask;
    std::optional<cache::WatermarkIdentity> identity;
};

/** The image as the public sees it after admission: dimensions of the public
 * canvas (reduced when a resolution limit applies), sample layout of the source
 * and the plan that produces a restricted canvas from the full source. */
struct IiifWorker::PublicImage {
    cache::MetadataPtr metadata;
    std::uint32_t width = 0, height = 0;
    bool native_gray = false;
    PixelLayout input_layout = PixelLayout::rgb;
    std::optional<iiif::ImageGeometry> restricted_plan;

    /** Original and watermarked JPEG renderings decode only the requested region
     * (see read_pixels); a restricted canvas decodes the whole image. */
    [[nodiscard]] bool decodes_jpeg_regions() const {
        return std::holds_alternative<JpegMetadata>(*metadata) && !restricted_plan;
    }

    /** Throws DeliveryLimitError if this rendering exceeds the decoding budget.
     * A restricted canvas is rendered from an in-memory raster, not the source. */
    void validate_budget(const iiif::ImageGeometry& geometry, const iiif::Rotation& rotation) const {
        if (restricted_plan) {
            const auto profile_bytes = std::visit([](const auto& value) { return value.icc_profile.size(); }, *metadata);
            validate_raster_delivery_budget(std::uint64_t{width} * height * 4, profile_bytes, geometry, rotation);
        } else {
            validate_delivery_budget(*metadata, geometry, rotation);
        }
    }
};

/** Result-cache state for one rendering. Member order matters: the lock is
 * released before the shared fill mutex it refers to can be destroyed. */
struct IiifWorker::CacheFill {
    std::optional<cache::CacheKey> key;   // Absent: render without the cache.
    std::shared_ptr<std::mutex> mutex;    // Serializes fills of this one key.
    std::unique_lock<std::mutex> lock;
    std::optional<std::string> hit;       // Complete cached encoding.
};

/** Authorize before accessing the definitive source. All blocking source/cache/
 * codec work runs on this worker, never on the I/O loop. */
drogon::HttpResponsePtr IiifWorker::resolve(const Request& request) {
    try {
        return process(request);
    } catch (...) {
        return failure_response(std::current_exception());
    }
}

drogon::HttpResponsePtr IiifWorker::process(const Request& request) {
    const auto& resource = resource_of(request.query);
    const auto allowed = authorize(request, resource);
    if (!allowed) return response(drogon::k403Forbidden, "access_denied", "Access to this image is denied");

    auto source = open_source(resource.identifier, allowed->source);
    // The mask is opened before any cache lookup: a missing or changed mask
    // can never be bypassed by an earlier encoded result.
    const auto watermark = allowed->watermark
        ? load_watermark(resource.identifier, *allowed->watermark) : Watermarking{};
    auto metadata = load_metadata(*source);
    if (!metadata)
        return response(drogon::k501NotImplemented, "source_format_not_supported",
                        "Only TIFF, JPEG 2000, JPEG and PNG sources are supported at present");
    auto admitted = admit(std::move(metadata), allowed->resolution);
    if (const auto* rejection = std::get_if<drogon::HttpResponsePtr>(&admitted)) return *rejection;
    const auto& image = std::get<PublicImage>(admitted);

    if (std::holds_alternative<iiif::InfoRequest>(request.query))
        return info_response(request, resource, image);
    return deliver_image(request, std::get<iiif::ImageRequest>(request.query), *source, image,
                         watermark, allowed->watermark);
}

/** Run the policy if one is configured. nullopt means an explicit deny; without
 * a policy everything is allowed with the default mapping. Policy failures
 * propagate as PreflightError and never become a decision. */
std::optional<preflight::Allow> IiifWorker::authorize(const Request& request, const iiif::Resource& resource) {
    if (!policy_) return preflight::Allow{};
    const auto kind = request.redirect_to_info ? preflight::RequestKind::base
        : std::holds_alternative<iiif::ImageRequest>(request.query)
            ? preflight::RequestKind::image : preflight::RequestKind::info;
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::steady_clock::now() - started; };
    try {
        auto decision = policy_->evaluate({resource, request.authorization, request.cookie, request.cookies,
                                           request.query_parameters, kind, request.method});
        if (std::holds_alternative<preflight::Deny>(decision)) {
            metrics_.preflight(Metrics::PreflightOutcome::deny, elapsed());
            return std::nullopt;
        }
        metrics_.preflight(Metrics::PreflightOutcome::allow, elapsed());
        return std::get<preflight::Allow>(std::move(decision));
    } catch (...) {
        metrics_.preflight(Metrics::PreflightOutcome::error, elapsed());
        throw;
    }
}

/** Main source and required mask share storage selection/opening semantics. */
std::unique_ptr<ByteSource> IiifWorker::open_source(std::string_view identifier,
                                                    const std::optional<SourceOverride>& selection) const {
    const auto selected = sources_.resolve_source(identifier, selection);
    if (const auto* local = std::get_if<LocalSource>(&selected)) {
        auto file = std::make_unique<LocalFileSource>(local->path);
        // Re-check containment on the opened descriptor: closes the window
        // between path resolution and open() (symlink swaps).
        require_opened_inside(*local, file->opened_path());
        return file;
    }
    if (!s3_client_) throw SourceError(SourceErrorCode::configuration, "S3 is not configured");
    const auto& remote = std::get<S3Source>(selected);
    return std::make_unique<S3ByteSource>(*s3_client_, remote.bucket, remote.key);
}

/** Open and decode (or reuse) the required mask. Every failure becomes
 * WatermarkError: delivery never falls back to an unmarked image. */
IiifWorker::Watermarking IiifWorker::load_watermark(std::string_view identifier,
                                                   const preflight::Watermark& policy) {
    try {
        const auto bytes = open_source(identifier, policy.source);
        cache::MaskPtr mask;
        {
            std::lock_guard lock(mask_mutex_);
            mask = mask_cache_.get(bytes->info());
        }
        if (mask) {
            spdlog::debug("IIIF mask cache hit");
        } else {
            spdlog::debug("IIIF mask cache miss");
            mask = std::make_shared<const RasterImage8>(read_png_mask(*bytes));
            try {
                std::lock_guard lock(mask_mutex_);
                if (!mask_cache_.put(bytes->info(), mask))
                    spdlog::debug("IIIF mask cache admission declined");
            } catch (const std::exception&) {
                // Only retention is optional; opening and decoding errors fail delivery.
                spdlog::warn("IIIF mask cache admission failed; using decoded mask");
            }
        }
        return {std::move(mask), cache::WatermarkIdentity{bytes->info(), policy.width_fraction,
                                                          policy.opacity, policy.color}};
    } catch (const std::exception&) {
        throw WatermarkError("Required watermark could not be loaded");
    }
}

/** Source header facts, shared across workers. Null for unrecognized formats;
 * reader errors propagate and are never cached. */
cache::MetadataPtr IiifWorker::load_metadata(const ByteSource& source) {
    {
        std::lock_guard lock(metadata_mutex_);
        if (auto cached = metadata_cache_.get(source.info())) {
            spdlog::debug("IIIF metadata cache hit");
            return cached;
        }
    }
    spdlog::debug("IIIF metadata cache miss");
    auto metadata = read_source_metadata(source);
    if (!metadata) return nullptr;
    try {
        std::lock_guard lock(metadata_mutex_);
        if (!metadata_cache_.put(source.info(), metadata))
            spdlog::debug("IIIF metadata cache admission declined");
    } catch (const std::exception&) {
        // Reading succeeded: inability to retain it must not fail delivery.
        spdlog::warn("IIIF metadata cache admission failed; using uncached metadata");
    }
    return metadata;
}

/** Decide whether the source can be delivered at all and describe its public
 * canvas. Admission promises at least the default full/max rendering; it uses
 * metadata and planning only, so corruption is still detected while reading. */
std::variant<IiifWorker::PublicImage, drogon::HttpResponsePtr> IiifWorker::admit(
        cache::MetadataPtr metadata, const std::optional<preflight::ResolutionLimit>& resolution) const {
    const auto not_deliverable = [](const char* message) {
        return response(drogon::k501NotImplemented, "source_not_deliverable", message);
    };
    PublicImage image;
    bool readable = false;
    bool transparency = false;
    if (const auto* tiff = std::get_if<TiffMetadata>(metadata.get())) {
        image.width = tiff->image.display_width();
        image.height = tiff->image.display_height();
        image.native_gray = tiff->image.is_grayscale();
        transparency = tiff->image.samples_per_pixel == 4;
        readable = supports_tiff_region(tiff->image);
    } else if (const auto* j2k = std::get_if<J2kMetadata>(metadata.get())) {
        image.width = j2k->width;
        image.height = j2k->height;
        image.native_gray = j2k->color == J2kColor::gray;
        readable = supports_j2k_pixels(*j2k);
    } else if (const auto* jpeg = std::get_if<JpegMetadata>(metadata.get())) {
        image.width = jpeg->width;
        image.height = jpeg->height;
        image.native_gray = jpeg->color == JpegColor::gray;
        readable = true; // The JPEG reader already enforces its supported subset.
    } else if (const auto* png = std::get_if<PngMetadata>(metadata.get())) {
        image.width = png->width;
        image.height = png->height;
        image.native_gray = png->color == PngColor::gray || png->color == PngColor::gray_alpha;
        transparency = png->transparency;
        readable = supports_png_pixels(*png);
    } else {
        return response(drogon::k501NotImplemented, "source_format_not_supported",
                        "Only TIFF, JPEG 2000, JPEG and PNG sources are supported at present");
    }
    if (!readable) return not_deliverable("Source layout or color encoding is not supported for delivery");
    // Gray sources keep alpha only for PNG; four-sample TIFF is RGBA.
    const bool png_alpha = std::holds_alternative<PngMetadata>(*metadata) && transparency;
    image.input_layout = image.native_gray ? (png_alpha ? PixelLayout::gray_alpha : PixelLayout::gray)
                                           : transparency ? PixelLayout::rgba : PixelLayout::rgb;
    image.metadata = std::move(metadata);

    if (resolution) {
        // A public canvas is independent of output limits. Do not silently
        // lower its resolution when server configuration changes.
        const auto geometry = iiif::resolve_geometry(iiif::FullRegion{}, iiif::Size{false, iiif::MaxSize{}},
            image.width, image.height, {false, resolution->max_width, resolution->max_height, std::nullopt});
        const auto* full = std::get_if<iiif::ImageGeometry>(&geometry);
        if (!full) throw DeliveryLimitError("Invalid public resolution");
        try { validate_delivery_budget(*image.metadata, *full, {false, 0}); }
        catch (const DeliveryLimitError&) {
            return not_deliverable("Public image cannot be generated within current decoding budgets");
        }
        image.restricted_plan = *full;
        image.width = full->output_width;
        image.height = full->output_height;
    }
    const auto maximum = iiif::resolve_geometry(iiif::FullRegion{}, iiif::Size{false, iiif::MaxSize{}},
                                                image.width, image.height, size_policy_);
    const auto* full = std::get_if<iiif::ImageGeometry>(&maximum);
    if (!full) return not_deliverable("Source has no deliverable maximum size");
    try { image.validate_budget(*full, {false, 0}); }
    catch (const DeliveryLimitError&) {
        return not_deliverable("Source cannot provide full/max within current decoding budgets");
    }
    return image;
}

/** info.json for the public canvas, or the 303 redirect of a base URI. */
drogon::HttpResponsePtr IiifWorker::info_response(const Request& request, const iiif::Resource& resource,
                                                  const PublicImage& image) const {
    auto document = make_info_document(resource, image.width, image.height, public_base_url_);
    document["maxWidth"] = Json::UInt(*size_policy_.max_width);
    document["maxHeight"] = Json::UInt(*size_policy_.max_height);
    document["maxArea"] = Json::UInt64(*size_policy_.max_area);
    document["extraFeatures"].append("canonicalLinkHeader");
    document["extraFeatures"].append("mirroring");
    document["extraFeatures"].append("profileLinkHeader");
    document["extraFeatures"].append("rotationArbitrary");
    if (size_policy_.allow_upscaling) document["extraFeatures"].append("sizeUpscaling");
    if (!image.native_gray) document["extraQualities"].append("color");
    document["extraFormats"].append("webp");
    document["extraQualities"].append("gray");
    document["extraQualities"].append("bitonal");
    if (request.redirect_to_info) {
        auto result = drogon::HttpResponse::newHttpResponse();
        result->setStatusCode(drogon::k303SeeOther);
        auto location = document["id"].asString() + "/info.json";
        if (!request.redirect_query.empty()) location += "?" + request.redirect_query;
        result->addHeader("Location", location);
        result->addHeader("Cache-Control", "no-store");
        return result;
    }
    const auto type = negotiate_info_type(request.accept
        ? std::optional<std::string_view>{*request.accept} : std::nullopt);
    if (!type) {
        auto result = response(drogon::k406NotAcceptable, "not_acceptable",
                               "Information is available as application/json or application/ld+json");
        result->addHeader("Vary", "Accept");
        return result;
    }
    auto result = drogon::HttpResponse::newHttpJsonResponse(document);
    result->setContentTypeString(*type == InfoMediaType::json_ld
        ? std::string(jsonld_content_type) : "application/json; charset=utf-8");
    result->addHeader("Vary", "Accept");
    result->addHeader("Cache-Control", "no-store");
    return result;
}

/** Output format and encoder settings of one image request. They are part of
 * the cache identity and are used unchanged by the encoder. */
struct IiifWorker::Encoding {
    iiif::Format format;
    bool webp_lossless;
    cache::ResultMetadata output;
};

/** Validate the request against the public canvas, then serve a cached result
 * or render, encode and publish a new one. */
drogon::HttpResponsePtr IiifWorker::deliver_image(const Request& request, const iiif::ImageRequest& image,
        const ByteSource& source, const PublicImage& canvas, const Watermarking& watermark,
        const std::optional<preflight::Watermark>& watermark_policy) {
    const auto geometry = iiif::resolve_geometry(image.region, image.size, canvas.width, canvas.height, size_policy_);
    if (const auto* error = std::get_if<iiif::GeometryError>(&geometry)) {
        switch (error->code) {
        case iiif::GeometryErrorCode::upscaling_disabled:
            return response(drogon::k501NotImplemented, "upscaling_disabled", "Image upscaling is disabled");
        case iiif::GeometryErrorCode::invalid_dimensions:
        case iiif::GeometryErrorCode::invalid_policy:
            return response(drogon::k500InternalServerError, "geometry_error", "Image geometry could not be resolved");
        default:
            return response(drogon::k400BadRequest, "invalid_iiif_geometry", error->message.c_str());
        }
    }
    const auto& plan = std::get<iiif::ImageGeometry>(geometry);
    spdlog::debug("Resolved IIIF crop {},{},{},{} -> {}x{}", plan.region.x, plan.region.y,
                  plan.region.width, plan.region.height, plan.output_width, plan.output_height);
    if (image.format != iiif::Format::jpg && image.format != iiif::Format::png && image.format != iiif::Format::webp)
        return response(drogon::k400BadRequest, "unsupported_format",
                        "Only JPEG, PNG and WebP output formats are supported");
    if (image.quality != iiif::Quality::default_quality && image.quality != iiif::Quality::color &&
        image.quality != iiif::Quality::gray && image.quality != iiif::Quality::bitonal)
        return response(drogon::k501NotImplemented, "not_implemented", "Unsupported image quality");

    // Source admission and request budgets both precede the cache lookup:
    // a stored result cannot grant capabilities or bypass policy.
    canvas.validate_budget(plan, image.rotation);
    const auto dimensions = rotated_size(plan.output_width, plan.output_height, image.rotation.degrees);
    const bool webp = image.format == iiif::Format::webp;
    if (webp && (dimensions.width > webp_maximum_dimension || dimensions.height > webp_maximum_dimension))
        return response(drogon::k400BadRequest, "unsupported_webp_dimensions", "WebP axes must not exceed 16383 pixels");
    const Encoding encoding{image.format,
        canvas.native_gray || image.quality == iiif::Quality::gray || image.quality == iiif::Quality::bitonal,
        {webp ? "image/webp" : image.format == iiif::Format::png ? "image/png" : "image/jpeg",
         dimensions.width, dimensions.height}};
    const auto canonical = canonical_image_uri(image, plan, canvas.width, canvas.height,
                                               size_policy_, canvas.native_gray, public_base_url_);

    auto fill = lookup_cached(source, canvas, plan, image, encoding, watermark, watermark_policy.has_value());
    if (fill.hit) return image_response(std::move(*fill.hit), encoding.output.media_type, canonical);

    // Decoding/encoding is the expensive part; skip it for a vanished client.
    // Returning releases any fill lock so another request may render.
    if (client_gone(request)) {
        metrics_.client_disconnected();
        return skipped_for_disconnected_client();
    }
    auto encoded = render(source, canvas, plan, image, encoding, watermark, watermark_policy);
    publish(fill, encoding.output, encoded);
    return image_response(std::move(encoded), encoding.output.media_type, canonical);
}

/** Build the cache key, take this key's fill lock and look for a stored result.
 * The lock is kept on a miss, so concurrent identical requests wait for one
 * rendering instead of duplicating it. Cache failures only disable caching for
 * this request; they never turn a policy/source/codec error into a hit. */
IiifWorker::CacheFill IiifWorker::lookup_cached(const ByteSource& source, const PublicImage& canvas,
        const iiif::ImageGeometry& plan, const iiif::ImageRequest& image, const Encoding& encoding,
        const Watermarking& watermark, bool watermarked) {
    CacheFill fill;
    if (!result_cache_) return fill;
    try {
        fill.key.emplace(cache::RenderIdentity{
            .source = source.info(),
            .variant = canvas.restricted_plan
                ? "restricted-v1:" + std::to_string(canvas.width) + "x" + std::to_string(canvas.height)
                : watermarked ? "watermarked" : "original",
            .geometry = plan,
            .rotation = image.rotation,
            .quality = image.quality,
            .input_layout = canvas.input_layout,
            .encoding = encoding.format == iiif::Format::webp
                ? cache::Encoding{cache::WebpEncoding{encoding.webp_lossless, webp_quality_}}
                : encoding.format == iiif::Format::png ? cache::Encoding{cache::PngEncoding{}}
                : cache::Encoding{jpeg_options_},
            .watermark = watermark.identity,
            .recipe = std::string(canvas.decodes_jpeg_regions() ? cache::jpeg_region_render_recipe
                                                                : cache::current_render_recipe),
            .bitonal_method = bitonal_method_
        });
        fill.mutex = fill_mutex(*fill.key);
        fill.lock = std::unique_lock<std::mutex>(*fill.mutex);
        if (const auto stored = result_cache_->get(*fill.key)) {
            const auto size = stored->bytes->info().size;
            if (stored->metadata != encoding.output || !size || size > encoded_limit)
                throw cache::CacheError("Cached result exceeds delivery contract");
            std::string encoded(static_cast<std::size_t>(size), '\0');
            if (stored->bytes->read_at(0, std::as_writable_bytes(std::span{encoded})) != size)
                throw cache::CacheError("Incomplete cached result");
            spdlog::debug("IIIF result cache hit");
            metrics_.result_cache_lookup(Metrics::CacheLookup::hit);
            fill.hit = std::move(encoded);
            return fill;
        }
        spdlog::debug("IIIF result cache miss");
        metrics_.result_cache_lookup(Metrics::CacheLookup::miss);
    } catch (const std::exception&) {
        fill.key.reset();
        if (fill.lock.owns_lock()) fill.lock.unlock();
        metrics_.result_cache_lookup(Metrics::CacheLookup::error);
        spdlog::warn("IIIF result cache lookup failed; rendering without cache");
    }
    return fill;
}

/** Decoded pixels covering a request, the exact crop window inside them and
 * the grid that places a watermark relative to the public canvas. */
struct IiifWorker::DecodedRegion {
    RasterImage8 pixels;
    PixelWindow window;
    WatermarkGrid grid;
};

/** Pixels of the requested region at the decoder resolution, with the exact
 * crop window. A restricted canvas is materialized once per source version
 * (shared through the variant cache) and every crop reads from it. */
IiifWorker::DecodedRegion IiifWorker::read_public_pixels(
        const ByteSource& source, const PublicImage& canvas, const iiif::ImageGeometry& plan) {
    if (!canvas.restricted_plan) {
        auto [pixels, window, grid] = read_pixels(source, *canvas.metadata, plan);
        return {std::move(pixels), window, grid};
    }
    // The source version and effective dimensions form the complete key.
    const cache::CacheKey key(cache::RenderIdentity{
        .source = source.info(), .variant = "restricted-source-v1", .geometry = *canvas.restricted_plan,
        .rotation = {false, 0}, .quality = iiif::Quality::default_quality,
        .input_layout = canvas.input_layout, .encoding = cache::PngEncoding{}, .watermark = std::nullopt});
    const auto fill = fill_mutex(key);
    std::unique_lock fill_lock(*fill);
    cache::VariantPtr reduced;
    {
        std::lock_guard lock(variant_mutex_);
        reduced = variant_cache_.get(key.serialized());
    }
    if (reduced) {
        spdlog::debug("IIIF variant cache hit");
    } else {
        spdlog::debug("IIIF variant cache miss");
        auto [full, full_window, grid] = read_pixels(source, *canvas.metadata, *canvas.restricted_plan);
        reduced = std::make_shared<const RasterImage8>(
            resize_image(std::move(full), full_window, canvas.width, canvas.height));
        try {
            std::lock_guard lock(variant_mutex_);
            if (!variant_cache_.put(key.serialized(), reduced))
                spdlog::debug("IIIF variant cache admission declined");
        } catch (const std::exception&) {
            spdlog::warn("IIIF variant cache admission failed; using reduced raster");
        }
    }
    // Copy for the owning processing pipeline; cached pixels remain immutable.
    // Watermark placement uses this public canvas too.
    return {*reduced, {{plan.region.x, plan.region.width, 1}, {plan.region.y, plan.region.height, 1}},
            {canvas.width, canvas.height}};
}

/** Decode, watermark, resize, rotate, convert quality and encode completely.
 * Output is buffered until encoding succeeds: a partial image is never a 200. */
std::string IiifWorker::render(const ByteSource& source, const PublicImage& canvas,
        const iiif::ImageGeometry& plan, const iiif::ImageRequest& image, const Encoding& encoding,
        const Watermarking& watermark, const std::optional<preflight::Watermark>& watermark_policy) {
    auto [pixels, window, watermark_grid] = read_public_pixels(source, canvas, plan);
    // Structured bindings name the DecodedRegion members; pixels is owned here.
    if (watermark.mask) {
        const auto color = watermark_color(pixels, watermark_policy->color);
        const auto mark = prepare_watermark(watermark.mask, canvas.width, canvas.height, watermark_policy->width_fraction);
        pixels = apply_watermark(std::move(pixels), mark, color, watermark_policy->opacity, watermark_grid);
    }
    pixels = resize_image(std::move(pixels), window, plan.output_width, plan.output_height);
    pixels = transform_image(std::move(pixels), image.rotation.mirrored, image.rotation.degrees);
    if (image.quality == iiif::Quality::gray || image.quality == iiif::Quality::bitonal) {
        if ((pixels.layout == PixelLayout::rgb || pixels.layout == PixelLayout::rgba) &&
            pixels.icc_profile.empty() && !pixels.srgb_intent)
            spdlog::debug("Gray conversion assumes sRGB for untagged RGB source");
        pixels = convert_to_gray(std::move(pixels));
    }
    if (image.quality == iiif::Quality::bitonal)
        pixels = convert_to_bitonal(std::move(pixels), bitonal_method_);

    std::string encoded;
    const auto sink = [&](std::span<const std::uint8_t> bytes) {
        if (bytes.size() > encoded_limit - encoded.size()) {
            if (encoding.format == iiif::Format::webp) throw WebpWriteError("Encoded WebP exceeds response limit");
            if (encoding.format == iiif::Format::png) throw PngWriteError("Encoded PNG exceeds response limit");
            throw JpegWriteError("Encoded JPEG exceeds response limit");
        }
        encoded.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    };
    switch (encoding.format) {
    case iiif::Format::webp:
        write_webp(pixels, sink, encoding.webp_lossless, webp_quality_);
        break;
    case iiif::Format::png:
        write_png(pixels, sink, image.quality == iiif::Quality::bitonal ? PngBitDepth::one : PngBitDepth::eight);
        break;
    default:
        pixels = composite_on_white(std::move(pixels));
        write_jpeg(pixels, sink, jpeg_options_);
        break;
    }
    return encoded;
}

/** Store a completed encoding if the lookup produced a key. Failures only
 * affect the cache; the response is delivered either way. */
void IiifWorker::publish(CacheFill& fill, const cache::ResultMetadata& output, const std::string& encoded) {
    if (!fill.key) return;
    try {
        const bool stored = result_cache_->put(*fill.key, output, std::as_bytes(std::span{encoded}));
        spdlog::debug(stored ? "IIIF result cache stored" : "IIIF result cache admission declined");
        metrics_.result_cache_store(stored ? Metrics::CacheStore::stored : Metrics::CacheStore::declined);
    } catch (const std::exception& error) {
        metrics_.result_cache_store(Metrics::CacheStore::error);
        // Several writes can already be in flight when the first fails.
        std::call_once(cache_write_error_, [&] {
            spdlog::error("IIIF result cache publication failed ({}); delivering the "
                "image uncached. Result-cache writes stay disabled until restart; "
                "existing entries are still served. Check free space and permissions "
                "of the cache directory.", error.what());
        });
    }
}

/** The single mapping from pipeline exceptions to the HTTP error contract.
 * Responses never contain credentials, Lua diagnostics or local paths. */
drogon::HttpResponsePtr IiifWorker::failure_response(std::exception_ptr failure) {
    try {
        std::rethrow_exception(failure);
    } catch (const preflight::PreflightError& error) {
        spdlog::error("IIIF preflight failed ({}; category {}): {}",
                      preflight::error_name(error.code()), static_cast<int>(error.code()), error.what());
        return response(drogon::k500InternalServerError, "preflight_failed", "Image access could not be evaluated");
    } catch (const WatermarkError&) {
        return response(drogon::k500InternalServerError,"watermark_failed","Required watermark could not be applied");
    } catch (const SourceError& error) {
        switch (error.code()) {
        case SourceErrorCode::configuration:
            return response(drogon::k500InternalServerError, "source_not_configured", error.what());
        case SourceErrorCode::invalid_path:
            return response(drogon::k403Forbidden, "source_not_allowed", "Image source is outside the allowed mapping");
        case SourceErrorCode::not_found:
            return response(drogon::k404NotFound, "image_not_found", "Image source was not found");
        case SourceErrorCode::io:
            return response(drogon::k500InternalServerError, "source_error", "Image source could not be accessed");
        }
    } catch (const ByteSourceError& error) {
        const auto failure = storage_failure(error.code());
        if (failure.retry_after) {
            spdlog::warn("IIIF source changed while it was read; client may retry");
            return retryable(failure.code, failure.message, *failure.retry_after);
        }
        return response(static_cast<drogon::HttpStatusCode>(failure.status), failure.code, failure.message);
    } catch (const PngUnsupportedError&) {
        return response(drogon::k501NotImplemented, "not_implemented", "PNG precision, color encoding, EXIF or animation is not supported");
    } catch (const PngReadError&) {
        return response(drogon::k500InternalServerError, "invalid_image_source", "PNG data could not be read");
    } catch (const JpegUnsupportedError&) {
        return response(drogon::k501NotImplemented, "not_implemented", "JPEG layout or orientation is not supported");
    } catch (const JpegReadError&) {
        return response(drogon::k500InternalServerError, "invalid_image_source", "JPEG data could not be read");
    } catch (const J2kUnsupportedError&) {
        return response(drogon::k501NotImplemented, "not_implemented", "JPEG 2000 pixel layout is not supported yet");
    } catch (const J2kReadError&) {
        return response(drogon::k500InternalServerError, "invalid_image_source", "JPEG 2000 data could not be read");
    } catch (const TiffUnsupportedError&) {
        return response(drogon::k501NotImplemented, "not_implemented", "TIFF pixel layout is not supported yet");
    } catch (const TiffReadError&) {
        return response(drogon::k500InternalServerError, "invalid_image_source", "TIFF data could not be read");
    } catch (const DeliveryLimitError&) {
        return response(drogon::k400BadRequest, "request_exceeds_working_limits",
                        "Requested rendering exceeds the current processing limits");
    } catch (const ColorConversionError&) {
        return response(drogon::k500InternalServerError, "color_conversion_failed", "Image color conversion could not be completed");
    } catch (const TransformError&) {
        return response(drogon::k500InternalServerError, "processing_failed", "Image mirroring or rotation could not be completed");
    } catch (const BitonalError&) {
        return response(drogon::k500InternalServerError, "processing_failed", "Bitonal conversion could not be completed");
    } catch (const CompositeError&) {
        return response(drogon::k500InternalServerError, "processing_failed", "Alpha compositing could not be completed");
    } catch (const ResizeError&) {
        return response(drogon::k500InternalServerError, "processing_failed", "Image resizing could not be completed");
    } catch (const WebpWriteError&) {
        return response(drogon::k500InternalServerError, "encoding_failed", "WebP output could not be produced");
    } catch (const PngWriteError&) {
        return response(drogon::k500InternalServerError, "encoding_failed", "PNG output could not be produced");
    } catch (const JpegWriteError&) {
        return response(drogon::k500InternalServerError, "encoding_failed", "JPEG output could not be produced");
    } catch (const std::exception& error) {
        // Unexpected failure: keep the cause for operators; clients get a generic 500.
        spdlog::error("IIIF request processing failed: {}", error.what());
    } catch (...) {
        spdlog::error("IIIF request processing failed with a non-standard exception");
    }

    return response(drogon::k500InternalServerError, "processing_failed", "Image request could not be processed");
}

} // namespace sipi::http
