#include "http/IiifWorker.h"
#include "http/IiifInfo.h"
#include <spdlog/spdlog.h>
#include <utility>
#include "storage/LocalFileSource.h"
#include "image/AreaResize.h"
#include <array>
#include <algorithm>
#include "imgformats/TiffReadPlan.h"
#include "imgformats/TiffMetadata.h"
#include "imgformats/TiffReader.h"
#include "imgformats/JpegWriter.h"

namespace sipi::http {
namespace {
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
                       iiif::SizePolicy size_policy, std::string public_base_url, std::optional<S3Client> s3_client)
    : policy_(std::move(policy)), sources_(std::move(sources)), s3_client_(std::move(s3_client)), size_policy_(size_policy),
      public_base_url_(std::move(public_base_url)),
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
 * then decode supported crops and encode JPEG on this worker, never the I/O loop. */
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

        // Identify classic TIFF/BigTIFF from bytes, not the filename extension.
        // This does not decode pixels. The metadata reader validates the header.
        std::array<unsigned char, 4> magic{};
        const auto count = source->read_at(0, std::as_writable_bytes(std::span{magic}));
        const bool tiff = count == magic.size() &&
            ((magic[0] == 'I' && magic[1] == 'I' && (magic[2] == 42 || magic[2] == 43) && magic[3] == 0) ||
             (magic[0] == 'M' && magic[1] == 'M' && magic[2] == 0 && (magic[3] == 42 || magic[3] == 43)));
        if (!tiff) return response(drogon::k501NotImplemented, "source_format_not_supported",
                                   "Only TIFF sources are supported at present");
        auto metadata = read_tiff_metadata(*source);
        if (std::holds_alternative<iiif::InfoRequest>(request.query)) {
            auto result = drogon::HttpResponse::newHttpJsonResponse(make_info_document(resource,
                metadata.image.display_width(), metadata.image.display_height(), public_base_url_));
            result->addHeader("Cache-Control", "no-store");
            return result;
        }
        if (const auto* image = std::get_if<iiif::ImageRequest>(&request.query)) {
            const auto geometry = iiif::resolve_geometry(image->region, image->size,
                metadata.image.display_width(), metadata.image.display_height(), size_policy_);
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
            if (image->format != iiif::Format::jpg ||
                (image->quality != iiif::Quality::default_quality && image->quality != iiif::Quality::color) ||
                image->rotation.mirrored || (image->rotation.degrees != 0 && image->rotation.degrees != 360) ||
                plan.output_width > plan.region.width || plan.output_height > plan.region.height)
                return response(drogon::k501NotImplemented, "not_implemented",
                    "Only unchanged or reduced RGB JPEG output without mirroring or rotation is supported");

            // Preserve base-image support rules and skip levels whose layouts
            // cannot be decoded. Corrupt selected data still fails visibly.
            if (!supports_tiff_region(metadata.image))
                throw TiffUnsupportedError("Unsupported TIFF main image layout");
            std::erase_if(metadata.levels, [](const auto& level) { return !supports_tiff_region(level); });
            const auto read_plan = plan_tiff_read(metadata, plan);
            spdlog::debug("TIFF directory {}: read {},{},{},{}", read_plan.directory.offset,
                read_plan.read_region.x, read_plan.read_region.y,
                read_plan.read_region.width, read_plan.read_region.height);
            auto pixels = read_tiff_region(*source, read_plan.directory, read_plan.read_region);
            pixels = downsample_area(std::move(pixels),
                PixelWindow{read_plan.horizontal, read_plan.vertical}, plan.output_width, plan.output_height);
            std::string encoded;
            // Buffer until encoding succeeds: never publish partial JPEGs as 200.
            // A later streaming/cache sink can use the same codec interface.
            constexpr std::size_t encoded_limit = 64 * 1024 * 1024;
            write_jpeg(pixels, [&](std::span<const std::uint8_t> bytes) {
                if (bytes.size() > encoded_limit - encoded.size())
                    throw JpegWriteError("Encoded JPEG exceeds response limit");
                encoded.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            });
            auto result = drogon::HttpResponse::newHttpResponse();
            result->setContentTypeCode(drogon::CT_IMAGE_JPG);
            result->addHeader("Cache-Control", "no-store");
            result->setBody(std::move(encoded));
            return result;
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
    } catch (const TiffUnsupportedError&) {
        return response(drogon::k501NotImplemented, "not_implemented", "TIFF pixel layout is not supported yet");
    } catch (const TiffReadError&) {
        return response(drogon::k500InternalServerError, "invalid_image_source", "TIFF data could not be read");
    } catch (const ResizeError&) {
        return response(drogon::k500InternalServerError, "processing_failed", "Image resizing could not be completed");
    } catch (const JpegWriteError&) {
        return response(drogon::k500InternalServerError, "encoding_failed", "JPEG output could not be produced");
    } catch (const std::exception&) {
        spdlog::error("IIIF request processing failed");
    }
    return response(drogon::k500InternalServerError, "processing_failed", "Image request could not be processed");
}

} // namespace sipi::http
