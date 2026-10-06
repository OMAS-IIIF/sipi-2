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
constexpr std::size_t encoded_limit = 64 * 1024 * 1024;
// Use the same explicit setting for identity and actual encoding.
constexpr int jpeg_quality = 90;

/** Build identical fresh/hit response headers; authorization is never cached. */
drogon::HttpResponsePtr image_response(std::string encoded, bool png, const std::string& canonical) {
    auto result = drogon::HttpResponse::newHttpResponse();
    result->setContentTypeCode(png ? drogon::CT_IMAGE_PNG : drogon::CT_IMAGE_JPG);
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
} // namespace

IiifWorker::IiifWorker(std::optional<preflight::LuaPreflight> policy, SourceResolver sources,
                       iiif::SizePolicy size_policy, std::string public_base_url, std::optional<S3Client> s3_client,
                       std::shared_ptr<cache::ResultCache> result_cache, cache::MetadataCacheLimits metadata_limits, unsigned worker_count, BitonalMethod bitonal_method)
    : policy_(std::move(policy)), sources_(std::move(sources)), s3_client_(std::move(s3_client)), size_policy_(bounded_output_policy(size_policy)),
      public_base_url_(std::move(public_base_url)), bitonal_method_(bitonal_method), result_cache_(std::move(result_cache)),
      metadata_cache_(metadata_limits) {
    if (!worker_count || worker_count > maximum_worker_count)
        throw std::invalid_argument("IIIF worker count must be between 1 and 32");
    spdlog::info("IIIF processing pool: {} workers", worker_count);
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
    if (stopping_ || jobs_.size() >= 64) return false;
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
        auto result = resolve(job.request);
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

/** Authorize before accessing the definitive source. Resolve metadata/geometry,
 * and capabilities before result-cache lookup. Render/fill on a miss, with all blocking
 * source/cache/codec work on this worker, never the I/O loop. */
drogon::HttpResponsePtr IiifWorker::resolve(const Request& request) {
    try {
        const auto& resource = std::visit([](const auto& query) -> const iiif::Resource& {
            return query.resource;
        }, request.query);
        std::optional<SourceOverride> override;
        if (policy_) {
            const auto decision = policy_->evaluate({resource, request.authorization,
                                                     request.cookie, request.cookies});
            if (std::holds_alternative<preflight::Deny>(decision))
                return response(drogon::k403Forbidden, "access_denied", "Access to this image is denied");
            override = std::get<preflight::Allow>(decision).source;
        }
        const auto selected = sources_.resolve_source(resource.identifier, override);
        std::unique_ptr<ByteSource> source;
        if (const auto* local = std::get_if<LocalSource>(&selected)) {
            source = std::make_unique<LocalFileSource>(local->path);
        } else {
            if (!s3_client_) throw SourceError(SourceErrorCode::configuration, "S3 is not configured");
            const auto& remote = std::get<S3Source>(selected);
            source = std::make_unique<S3ByteSource>(*s3_client_, remote.bucket, remote.key);
        }

        cache::MetadataPtr metadata;
        {
            std::lock_guard lock(metadata_mutex_);
            metadata = metadata_cache_.get(source->info());
        }
        if (metadata) {
            spdlog::debug("IIIF metadata cache hit");
        } else {
            spdlog::debug("IIIF metadata cache miss");
            metadata = read_source_metadata(*source);
            if (!metadata)
                return response(drogon::k501NotImplemented, "source_format_not_supported",
                                "Only TIFF, JPEG 2000, JPEG and PNG sources are supported at present");
            try {
                std::lock_guard lock(metadata_mutex_);
                if (!metadata_cache_.put(source->info(), metadata))
                    spdlog::debug("IIIF metadata cache admission declined");
            } catch (const std::exception&) {
                // Reading succeeded: inability to retain it must not fail delivery.
                spdlog::warn("IIIF metadata cache admission failed; using uncached metadata");
            }
        }
        const auto* tiff_metadata = std::get_if<TiffMetadata>(metadata.get());
        const auto* j2k_metadata = std::get_if<J2kMetadata>(metadata.get());
        const auto* jpeg_metadata = std::get_if<JpegMetadata>(metadata.get());
        const auto* png_metadata = std::get_if<PngMetadata>(metadata.get());
        bool readable = false;
        std::uint32_t width, height;
        bool native_gray = false;
        if (tiff_metadata) {
            width = tiff_metadata->image.display_width();
            height = tiff_metadata->image.display_height();
            native_gray = tiff_metadata->image.is_grayscale();
            readable = supports_tiff_region(tiff_metadata->image);
        } else if (j2k_metadata) {
            width = j2k_metadata->width;
            height = j2k_metadata->height;
            native_gray = j2k_metadata->color == J2kColor::gray;
            readable = supports_j2k_pixels(*j2k_metadata);
        } else if (jpeg_metadata) {
            width = jpeg_metadata->width; height = jpeg_metadata->height;
            native_gray = jpeg_metadata->color == JpegColor::gray;
            readable = true; // The JPEG reader already enforces its supported subset.
        } else if (png_metadata) {
            width = png_metadata->width; height = png_metadata->height;
            native_gray = png_metadata->color == PngColor::gray || png_metadata->color == PngColor::gray_alpha;
            readable = supports_png_pixels(*png_metadata);
        } else {
            return response(drogon::k501NotImplemented, "source_format_not_supported",
                            "Only TIFF, JPEG 2000, JPEG and PNG sources are supported at present");
        }
        if (!readable)
            return response(drogon::k501NotImplemented, "source_not_deliverable",
                            "Source layout or color encoding is not supported for delivery");
        // Admission promises at least the default full/max rendering. This uses
        // metadata/planning only; corruption remains detectable during actual reads.
        const auto maximum = iiif::resolve_geometry(iiif::FullRegion{},
            iiif::Size{false, iiif::MaxSize{}}, width, height, size_policy_);
        if (const auto* full = std::get_if<iiif::ImageGeometry>(&maximum)) {
            try { validate_delivery_budget(*metadata, *full, {false,0}); }
            catch (const DeliveryLimitError&) {
                return response(drogon::k501NotImplemented, "source_not_deliverable",
                    "Source cannot provide full/max within current decoding budgets");
            }
        } else {
            return response(drogon::k501NotImplemented, "source_not_deliverable",
                            "Source has no deliverable maximum size");
        }
        if (std::holds_alternative<iiif::InfoRequest>(request.query)) {
            auto document = make_info_document(resource,
                width, height, public_base_url_);
            document["maxWidth"] = Json::UInt(*size_policy_.max_width);
            document["maxHeight"] = Json::UInt(*size_policy_.max_height);
            document["maxArea"] = Json::UInt64(*size_policy_.max_area);
            document["extraFeatures"].append("canonicalLinkHeader");
            document["extraFeatures"].append("mirroring");
            document["extraFeatures"].append("profileLinkHeader");
            document["extraFeatures"].append("rotationArbitrary");
            if (!native_gray) document["extraQualities"].append("color");
            document["extraQualities"].append("gray");
            document["extraQualities"].append("bitonal");
            if (request.redirect_to_info) {
                auto result = drogon::HttpResponse::newHttpResponse();
                result->setStatusCode(drogon::k303SeeOther);
                result->addHeader("Location", document["id"].asString()+"/info.json");
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
        if (const auto* image = std::get_if<iiif::ImageRequest>(&request.query)) {
            const auto geometry = iiif::resolve_geometry(image->region, image->size,
                width, height, size_policy_);
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
            if (image->format != iiif::Format::jpg && image->format != iiif::Format::png)
                return response(drogon::k400BadRequest, "unsupported_format",
                                "Only JPEG and PNG output formats are supported");
            if ((image->quality != iiif::Quality::default_quality && image->quality != iiif::Quality::color &&
                 image->quality != iiif::Quality::gray && image->quality != iiif::Quality::bitonal) ||
                plan.output_width > plan.region.width || plan.output_height > plan.region.height)
                return response(drogon::k501NotImplemented, "not_implemented",
                    "Only unchanged or reduced RGB/RGBA/gray JPEG/PNG output is supported");

            // Source admission above and request budgets below both precede cache
            // lookup: a stored result cannot grant capabilities or bypass policy.
            const bool png = image->format == iiif::Format::png;
            validate_delivery_budget(*metadata, plan, image->rotation);
            const auto dimensions = rotated_size(plan.output_width, plan.output_height, image->rotation.degrees);
            const cache::ResultMetadata output{png ? "image/png" : "image/jpeg", dimensions.width, dimensions.height};
            const auto canonical = canonical_image_uri(*image, plan, width, height,
                                                       size_policy_, native_gray, public_base_url_);
            std::optional<cache::CacheKey> cache_key;
            // Keep the shared mutex alive until after its lock is released, including
            // exception exits. Recheck the cache after any previous fill completes.
            std::shared_ptr<std::mutex> fill;
            std::unique_lock<std::mutex> fill_lock;
            if (result_cache_) {
                try {
                    const auto layout = native_gray ?
                        ((png_metadata && png_metadata->transparency) ? PixelLayout::gray_alpha : PixelLayout::gray) :
                        (png_metadata && png_metadata->transparency) ? PixelLayout::rgba :
                        (tiff_metadata && tiff_metadata->image.samples_per_pixel == 4
                            ? PixelLayout::rgba : PixelLayout::rgb);
                    cache_key.emplace(cache::RenderIdentity{
                        .source = source->info(),
                        // Current Lua policy supports allow/deny/source selection only.
                        // Future reduced/watermarked variants must supply their actual identity.
                        .variant = "original",
                        .geometry = plan,
                        .rotation = image->rotation,
                        .quality = image->quality,
                        .input_layout = layout,
                        .encoding = png ? cache::Encoding{cache::PngEncoding{}}
                                        : cache::Encoding{cache::JpegEncoding{jpeg_quality}},
                        .watermark = std::nullopt,
                        .bitonal_method = bitonal_method_
                    });
                    fill = fill_mutex(*cache_key);
                    fill_lock = std::unique_lock<std::mutex>(*fill);
                    if (const auto hit = result_cache_->get(*cache_key)) {
                        const auto size = hit->bytes->info().size;
                        if (hit->metadata != output || !size || size > encoded_limit)
                            throw cache::CacheError("Cached result exceeds delivery contract");
                        std::string encoded(static_cast<std::size_t>(size), '\0');
                        if (hit->bytes->read_at(0, std::as_writable_bytes(std::span{encoded})) != size)
                            throw cache::CacheError("Incomplete cached result");
                        spdlog::debug("IIIF result cache hit");
                        return image_response(std::move(encoded), png, canonical);
                    }
                    spdlog::debug("IIIF result cache miss");
                } catch (const std::exception&) {
                    // Only cache operations live in this boundary. Never convert
                    // a policy/source/codec error into permission to reuse a hit.
                    cache_key.reset();
                    if (fill_lock.owns_lock()) fill_lock.unlock();
                    spdlog::warn("IIIF result cache lookup failed; rendering without cache");
                }
            }

            // Each format selects and reads its own resolution. The owning raster
            // and exact window then enter one shared processing/encoding pipeline.
            auto [pixels, window] = [&]() -> std::pair<RasterImage8, PixelWindow> {
                if (tiff_metadata) {
                    const auto read_plan = supported_tiff_plan(*tiff_metadata, plan);
                    return {read_tiff_region(*source, read_plan.directory, read_plan.read_region),
                            {read_plan.horizontal, read_plan.vertical}};
                }
                if (png_metadata) {
                    return {read_png_image(*source, *png_metadata),
                        {{plan.region.x, plan.region.width, 1}, {plan.region.y, plan.region.height, 1}}};
                }
                if (jpeg_metadata) {
                    // Decode the whole JPEG; the shared window resampler crops and
                    // scales without an additional intermediate crop buffer.
                    return {read_jpeg_image(*source, *jpeg_metadata),
                        {{plan.region.x, plan.region.width, 1}, {plan.region.y, plan.region.height, 1}}};
                }
                const auto read_plan = plan_j2k_read(*j2k_metadata, plan);
                return {read_j2k_region(*source, *j2k_metadata, read_plan), read_plan.window};
            }();
            pixels = downsample_area(std::move(pixels), window, plan.output_width, plan.output_height);
            pixels = transform_image(std::move(pixels), image->rotation.mirrored, image->rotation.degrees);
            if (image->quality == iiif::Quality::gray || image->quality == iiif::Quality::bitonal) {
                if ((pixels.layout == PixelLayout::rgb || pixels.layout == PixelLayout::rgba) &&
                    pixels.icc_profile.empty() && !pixels.srgb_intent)
                    spdlog::debug("Gray conversion assumes sRGB for untagged RGB source");
                pixels = convert_to_gray(std::move(pixels));
            }
            if (image->quality == iiif::Quality::bitonal)
                pixels = convert_to_bitonal(std::move(pixels), bitonal_method_);
            std::string encoded;
            // Buffer until encoding succeeds: never publish partial images as 200.
            // Both cache fill and delivery consume only the completed encoding.
            const auto sink = [&](std::span<const std::uint8_t> bytes) {
                if (bytes.size() > encoded_limit - encoded.size()) {
                    if (png) throw PngWriteError("Encoded PNG exceeds response limit");
                    throw JpegWriteError("Encoded JPEG exceeds response limit");
                }
                encoded.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            };
            if (png) write_png(pixels, sink, image->quality == iiif::Quality::bitonal
                ? PngBitDepth::one : PngBitDepth::eight);
            else {
                pixels = composite_on_white(std::move(pixels));
                write_jpeg(pixels, sink, jpeg_quality);
            }
            if (cache_key) {
                try {
                    const bool stored = result_cache_->put(*cache_key, output,
                        std::as_bytes(std::span{encoded}));
                    spdlog::debug(stored ? "IIIF result cache stored" : "IIIF result cache admission declined");
                } catch (const std::exception&) {
                    spdlog::warn("IIIF result cache publication failed; delivering uncached image");
                }
            }
            return image_response(std::move(encoded), png, canonical);
        }
        return response(drogon::k500InternalServerError, "processing_failed", "Unexpected IIIF request type");
    } catch (const preflight::PreflightError& error) {
        spdlog::error("IIIF preflight failed (category {})", static_cast<int>(error.code()));
        return response(drogon::k500InternalServerError, "preflight_failed", "Image access could not be evaluated");
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
        if (error.code() == ByteSourceErrorCode::not_found)
            return response(drogon::k404NotFound, "image_not_found", "Image source was not found");
        return response(drogon::k500InternalServerError, "source_error", "Image source could not be read consistently");
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
    } catch (const PngWriteError&) {
        return response(drogon::k500InternalServerError, "encoding_failed", "PNG output could not be produced");
    } catch (const JpegWriteError&) {
        return response(drogon::k500InternalServerError, "encoding_failed", "JPEG output could not be produced");
    } catch (const std::exception&) {
        spdlog::error("IIIF request processing failed");
    }
    return response(drogon::k500InternalServerError, "processing_failed", "Image request could not be processed");
}

} // namespace sipi::http
