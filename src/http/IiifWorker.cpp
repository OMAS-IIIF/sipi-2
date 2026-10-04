#include "http/IiifWorker.h"
#include "http/IiifInfo.h"
#include <spdlog/spdlog.h>
#include <utility>
#include "storage/LocalFileSource.h"
#include "image/AreaResize.h"
#include "image/AlphaComposite.h"
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
constexpr std::size_t encoded_limit = 64 * 1024 * 1024;
// Use the same explicit setting for identity and actual encoding.
constexpr int jpeg_quality = 90;

/** Build identical fresh/hit response headers; authorization is never cached. */
drogon::HttpResponsePtr image_response(std::string encoded, bool png) {
    auto result = drogon::HttpResponse::newHttpResponse();
    result->setContentTypeCode(png ? drogon::CT_IMAGE_PNG : drogon::CT_IMAGE_JPG);
    result->addHeader("Cache-Control", "no-store");
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
                       std::shared_ptr<cache::ResultCache> result_cache)
    : policy_(std::move(policy)), sources_(std::move(sources)), s3_client_(std::move(s3_client)), size_policy_(size_policy),
      public_base_url_(std::move(public_base_url)), result_cache_(std::move(result_cache)),
      thread_([this] { run(); }) {}

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
    ready_.notify_one();
    if (thread_.joinable()) thread_.join();
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
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
        }
        job.callback(result);
    }
}

/** Authorize before accessing the definitive source. Resolve metadata/geometry,
 * and capabilities before cache lookup. Render/fill on a miss, with all blocking
 * source/cache/codec work on this worker, never the I/O loop. */
drogon::HttpResponsePtr IiifWorker::resolve(const Request& request) const {
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

        // Identify TIFF, JP2 or raw J2K from bytes, not the filename extension.
        // This does not decode pixels. The metadata reader validates the header.
        std::array<unsigned char, 12> magic{};
        const auto count = source->read_at(0, std::as_writable_bytes(std::span{magic}));
        const bool tiff = count >= 4 &&
            ((magic[0] == 'I' && magic[1] == 'I' && (magic[2] == 42 || magic[2] == 43) && magic[3] == 0) ||
             (magic[0] == 'M' && magic[1] == 'M' && magic[2] == 0 && (magic[3] == 42 || magic[3] == 43)));
        constexpr std::array<unsigned char, 12> jp2_magic{0,0,0,12,'j','P',' ', ' ',13,10,135,10};
        const bool j2k = (count >= 4 && magic[0] == 0xff && magic[1] == 0x4f &&
                          magic[2] == 0xff && magic[3] == 0x51) ||
                         (count == magic.size() && magic == jp2_magic);
        std::optional<TiffMetadata> tiff_metadata;
        std::optional<J2kMetadata> j2k_metadata;
        bool readable_gray = false;
        std::uint32_t width, height;
        bool native_gray = false;
        if (tiff) {
            tiff_metadata = read_tiff_metadata(*source);
            width = tiff_metadata->image.display_width();
            height = tiff_metadata->image.display_height();
            native_gray = tiff_metadata->image.is_grayscale();
            readable_gray = native_gray && supports_tiff_region(tiff_metadata->image);
        } else if (j2k) {
            j2k_metadata = read_j2k_metadata(*source);
            width = j2k_metadata->width;
            height = j2k_metadata->height;
            native_gray = j2k_metadata->color == J2kColor::gray;
            readable_gray = native_gray && supports_j2k_pixels(*j2k_metadata);
        } else {
            return response(drogon::k501NotImplemented, "source_format_not_supported",
                            "Only TIFF and JPEG 2000 sources are supported at present");
        }
        if (std::holds_alternative<iiif::InfoRequest>(request.query)) {
            auto document = make_info_document(resource,
                width, height, public_base_url_);
            if (readable_gray)
                document["extraQualities"].append("gray");
            auto result = drogon::HttpResponse::newHttpJsonResponse(document);
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
            if ((image->format != iiif::Format::jpg && image->format != iiif::Format::png) ||
                (image->quality != iiif::Quality::default_quality && image->quality != iiif::Quality::color &&
                 !(image->quality == iiif::Quality::gray && native_gray)) ||
                image->rotation.mirrored || (image->rotation.degrees != 0 && image->rotation.degrees != 360) ||
                plan.output_width > plan.region.width || plan.output_height > plan.region.height)
                return response(drogon::k501NotImplemented, "not_implemented",
                    "Only unchanged or reduced RGB/RGBA/gray JPEG/PNG output without mirroring or rotation is supported");

            // A ready entry cannot grant capabilities that today's renderer lacks.
            // Check source layout before lookup, as well as IIIF operations above.
            if (tiff_metadata && !supports_tiff_region(tiff_metadata->image))
                throw TiffUnsupportedError("Unsupported TIFF main image layout");
            if (j2k_metadata && !supports_j2k_pixels(*j2k_metadata))
                throw J2kUnsupportedError("Unsupported JPEG 2000 pixel layout");

            const bool png = image->format == iiif::Format::png;
            const cache::ResultMetadata output{png ? "image/png" : "image/jpeg",
                                               plan.output_width, plan.output_height};
            std::optional<cache::CacheKey> cache_key;
            if (result_cache_) {
                try {
                    const auto layout = native_gray ? PixelLayout::gray :
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
                        .watermark = std::nullopt
                    });
                    if (const auto hit = result_cache_->get(*cache_key)) {
                        const auto size = hit->bytes->info().size;
                        if (hit->metadata != output || !size || size > encoded_limit)
                            throw cache::CacheError("Cached result exceeds delivery contract");
                        std::string encoded(static_cast<std::size_t>(size), '\0');
                        if (hit->bytes->read_at(0, std::as_writable_bytes(std::span{encoded})) != size)
                            throw cache::CacheError("Incomplete cached result");
                        spdlog::debug("IIIF result cache hit");
                        return image_response(std::move(encoded), png);
                    }
                    spdlog::debug("IIIF result cache miss");
                } catch (const std::exception&) {
                    // Only cache operations live in this boundary. Never convert
                    // a policy/source/codec error into permission to reuse a hit.
                    cache_key.reset();
                    spdlog::warn("IIIF result cache lookup failed; rendering without cache");
                }
            }

            // Each format selects and reads its own resolution. The owning raster
            // and exact window then enter one shared processing/encoding pipeline.
            auto [pixels, window] = [&]() -> std::pair<RasterImage8, PixelWindow> {
                if (tiff_metadata) {
                    std::erase_if(tiff_metadata->levels,
                        [](const auto& level) { return !supports_tiff_region(level); });
                    const auto read_plan = plan_tiff_read(*tiff_metadata, plan);
                    return {read_tiff_region(*source, read_plan.directory, read_plan.read_region),
                            {read_plan.horizontal, read_plan.vertical}};
                }
                const auto read_plan = plan_j2k_read(*j2k_metadata, plan);
                return {read_j2k_region(*source, *j2k_metadata, read_plan), read_plan.window};
            }();
            pixels = downsample_area(std::move(pixels), window, plan.output_width, plan.output_height);
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
            if (png) write_png(pixels, sink);
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
            return image_response(std::move(encoded), png);
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
    } catch (const J2kUnsupportedError&) {
        return response(drogon::k501NotImplemented, "not_implemented", "JPEG 2000 pixel layout is not supported yet");
    } catch (const J2kReadError&) {
        return response(drogon::k500InternalServerError, "invalid_image_source", "JPEG 2000 data could not be read");
    } catch (const TiffUnsupportedError&) {
        return response(drogon::k501NotImplemented, "not_implemented", "TIFF pixel layout is not supported yet");
    } catch (const TiffReadError&) {
        return response(drogon::k500InternalServerError, "invalid_image_source", "TIFF data could not be read");
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
