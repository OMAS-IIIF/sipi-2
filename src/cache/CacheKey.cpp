#include "CacheKey.h"
#include <openssl/evp.h>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace sipi::cache {
namespace {
std::string decimal(std::uint64_t value) {
    std::array<char, 20> buffer{}; // UINT64_MAX has 20 decimal digits.
    const auto result = std::to_chars(buffer.data(), buffer.data()+buffer.size(), value);
    return {buffer.data(), result.ptr};
}

/** Lengths count bytes, not characters. Fixed labels are part of the schema. */
void field(std::string& result, std::string_view name, std::string_view value) {
    result.append(name).append(":").append(decimal(value.size())).append(":");
    result.append(value).push_back('\n');
}
void number(std::string& result, std::string_view name, std::uint64_t value) {
    field(result, name, decimal(value));
}
void validate_source(const SourceInfo& source) {
    if (source.identity.empty() || source.version.empty() || !source.size)
        throw std::invalid_argument("Cache identity requires a nonempty version-bound source");
}

std::string binary64(double value) {
    static_assert(sizeof(double) == sizeof(std::uint64_t) && std::numeric_limits<double>::is_iec559);
    // Both +0 and -0 compare equal; canonicalize before preserving exact bits.
    if (value == 0) value = 0;
    const auto bits = std::bit_cast<std::uint64_t>(value);
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result(16, '0');
    for (unsigned i=0; i<16; ++i) result[i] = digits[(bits >> (60-4*i)) & 15];
    return result;
}

std::string angle(double degrees) {
    if (!std::isfinite(degrees) || degrees < 0 || degrees > 360)
        throw std::invalid_argument("Invalid cache rotation");
    return binary64(degrees == 360 ? 0 : degrees);
}

std::string_view effective_quality(iiif::Quality quality, PixelLayout layout) {
    if (layout != PixelLayout::gray && layout != PixelLayout::rgb && layout != PixelLayout::rgba && layout != PixelLayout::gray_alpha)
        throw std::invalid_argument("Invalid cache input layout");
    switch (quality) {
    case iiif::Quality::default_quality:
    case iiif::Quality::color: return (layout == PixelLayout::gray || layout == PixelLayout::gray_alpha) ? "gray" : "color";
    case iiif::Quality::gray: return "gray";
    case iiif::Quality::bitonal: return "bitonal";
    }
    throw std::invalid_argument("Invalid cache quality");
}

std::string serialize(const RenderIdentity& value) {
    validate_source(value.source);
    if (value.watermark) {
        validate_source(value.watermark->source);
        if (!std::isfinite(value.watermark->width_fraction) || value.watermark->width_fraction <= 0 ||
            value.watermark->width_fraction > 1 || !std::isfinite(value.watermark->opacity) ||
            value.watermark->opacity < 0 || value.watermark->opacity > 1)
            throw std::invalid_argument("Invalid watermark cache settings");
    }
    if (value.variant.empty() || value.recipe.empty())
        throw std::invalid_argument("Cache variant and rendering recipe must be explicit");
    if (value.bitonal_method != BitonalMethod::threshold &&
        value.bitonal_method != BitonalMethod::floyd_steinberg)
        throw std::invalid_argument("Invalid cache bitonal method");
    const auto& crop = value.geometry.region;
    if (!crop.width || !crop.height || !value.geometry.output_width || !value.geometry.output_height ||
        std::uint64_t{crop.x}+crop.width > UINT32_MAX || std::uint64_t{crop.y}+crop.height > UINT32_MAX)
        throw std::invalid_argument("Invalid cache geometry");

    std::string result = "sipi-result-key-v1\n";
    field(result,"source.identity",value.source.identity);
    field(result,"source.version",value.source.version);
    number(result,"source.bytes",value.source.size);
    field(result,"variant",value.variant);
    number(result,"watermark.present",value.watermark.has_value());
    if (value.watermark) {
        field(result,"watermark.identity",value.watermark->source.identity);
        field(result,"watermark.version",value.watermark->source.version);
        number(result,"watermark.bytes",value.watermark->source.size);
        field(result,"watermark.width.binary64",binary64(value.watermark->width_fraction));
        field(result,"watermark.opacity.binary64",binary64(value.watermark->opacity));
        for (unsigned i=0;i<3;++i)
            number(result,std::string("watermark.srgb.")+decimal(i),value.watermark->color[i]);
        field(result,"watermark.recipe","center-fit-direct-area-linear-srgb-bpc-v1");
    }
    number(result,"crop.x",crop.x); number(result,"crop.y",crop.y);
    number(result,"crop.width",crop.width); number(result,"crop.height",crop.height);
    number(result,"size.width",value.geometry.output_width);
    number(result,"size.height",value.geometry.output_height);
    number(result,"mirror",value.rotation.mirrored);
    field(result,"rotation.binary64",angle(value.rotation.degrees));
    field(result,"quality",effective_quality(value.quality,value.input_layout));
    if (const auto* jpeg = std::get_if<JpegEncoding>(&value.encoding)) {
        if (jpeg->quality < 1 || jpeg->quality > 100)
            throw std::invalid_argument("Invalid cache JPEG encoder quality");
        field(result,"encoding","jpeg");
        number(result,"jpeg.quality",static_cast<unsigned>(jpeg->quality));
    } else if (const auto* webp = std::get_if<WebpEncoding>(&value.encoding)) {
        if (webp->quality < 1 || webp->quality > 100)
            throw std::invalid_argument("Invalid cache WebP quality");
        field(result,"encoding","webp");
        number(result,"webp.lossless",webp->lossless);
        number(result,"webp.quality",static_cast<unsigned>(webp->quality));
        field(result,"webp.recipe","libwebp-1.6.0-method4-exact-gray-srgb-v1");
    } else {
        field(result,"encoding","png");
    }
    // Absent means the original threshold policy, preserving existing v1 keys.
    // Only bitonal output depends on this setting.
    if (value.quality == iiif::Quality::bitonal &&
        value.bitonal_method == BitonalMethod::floyd_steinberg)
        field(result,"bitonal.method","floyd-steinberg");
    field(result,"recipe",value.recipe);
    return result;
}

std::string sha256(std::string_view bytes) {
    std::array<unsigned char, 32> digest{};
    unsigned int size = 0;
    if (EVP_Digest(bytes.data(),bytes.size(),digest.data(),&size,EVP_sha256(),nullptr) != 1 ||
        size != digest.size())
        throw std::runtime_error("Cache SHA-256 calculation failed");
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : digest) {
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}
} // namespace

CacheKey::CacheKey(const RenderIdentity& identity)
    : serialized_(serialize(identity)), digest_(sha256(serialized_)) {}
} // namespace sipi::cache
