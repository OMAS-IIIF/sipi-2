#include "IIIFParser.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <optional>
#include <utility>

namespace sipi::iiif {
namespace {

bool is_digit(char c) { return c >= '0' && c <= '9'; }

/** Require the whole token: from_chars alone would accept a numeric prefix. */
std::optional<std::uint32_t> pixel(std::string_view token) {
    if (token.empty() || !std::all_of(token.begin(), token.end(), is_digit)) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (error != std::errc{} || end != token.data() + token.size()) {
        return std::nullopt;
    }
    return value;
}

/** IIIF decimals allow digits and one dot, but no sign, exponent, or whitespace.
 * Noncanonical spellings such as .5 and 01.00 are still valid syntax. */
std::optional<double> decimal(std::string_view token) {
    bool digit_seen = false;
    bool dot_seen = false;
    for (const char c : token) {
        if (is_digit(c)) {
            digit_seen = true;
        } else if (c == '.' && !dot_seen) {
            dot_seen = true;
        } else {
            return std::nullopt;
        }
    }
    if (!digit_seen) {
        return std::nullopt;
    }
    double value = 0;
    const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value,
                                             std::chars_format::fixed);
    if (error != std::errc{} || end != token.data() + token.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

/** Split an exact number of comma-separated fields without allocating strings. */
template<std::size_t N>
std::optional<std::array<std::string_view, N>> fields(std::string_view token) {
    std::array<std::string_view, N> result;
    for (std::size_t i = 0; i < N - 1; ++i) {
        const auto comma = token.find(',');
        if (comma == std::string_view::npos) {
            return std::nullopt;
        }
        result[i] = token.substr(0, comma);
        token.remove_prefix(comma + 1);
    }
    if (token.find(',') != std::string_view::npos) {
        return std::nullopt;
    }
    result[N - 1] = token;
    return result;
}

std::optional<Region> parse_region(std::string_view token) {
    if (token == "full") { return FullRegion{}; }
    if (token == "square") { return SquareRegion{}; }
    const bool percent = token.starts_with("pct:");
    if (percent) { token.remove_prefix(4); }
    const auto parts = fields<4>(token);
    if (!parts) { return std::nullopt; }
    if (percent) {
        const auto x = decimal((*parts)[0]), y = decimal((*parts)[1]);
        const auto w = decimal((*parts)[2]), h = decimal((*parts)[3]);
        if (x && y && w && h && *w > 0 && *h > 0) {
            return PercentRegion{*x, *y, *w, *h};
        }
    } else {
        const auto x = pixel((*parts)[0]), y = pixel((*parts)[1]);
        const auto w = pixel((*parts)[2]), h = pixel((*parts)[3]);
        if (x && y && w && h && *w > 0 && *h > 0) {
            return PixelRegion{*x, *y, *w, *h};
        }
    }
    return std::nullopt;
}

std::optional<Size> parse_size(std::string_view token) {
    const bool upscale = token.starts_with('^');
    if (upscale) { token.remove_prefix(1); }
    if (token == "max") { return Size{upscale, MaxSize{}}; }
    if (token.starts_with("pct:")) {
        const auto value = decimal(token.substr(4));
        if (value && *value > 0 && (upscale || *value <= 100)) {
            return Size{upscale, PercentSize{*value}};
        }
        return std::nullopt;
    }
    const bool fit = token.starts_with('!');
    if (fit) { token.remove_prefix(1); }
    const auto parts = fields<2>(token);
    if (!parts) { return std::nullopt; }
    const auto width = pixel((*parts)[0]), height = pixel((*parts)[1]);
    if (width && height && *width > 0 && *height > 0) {
        if (fit) { return Size{upscale, FitSize{*width, *height}}; }
        return Size{upscale, ExactSize{*width, *height}};
    }
    if (!fit && width && *width > 0 && (*parts)[1].empty()) {
        return Size{upscale, WidthSize{*width}};
    }
    if (!fit && height && *height > 0 && (*parts)[0].empty()) {
        return Size{upscale, HeightSize{*height}};
    }
    return std::nullopt;
}

std::optional<Rotation> parse_rotation(std::string_view token) {
    const bool mirrored = token.starts_with('!');
    if (mirrored) { token.remove_prefix(1); }
    const auto value = decimal(token);
    if (!value || *value > 360) { return std::nullopt; }
    return Rotation{mirrored, *value};
}

/** Map exact protocol names to enums; unsupported names stay explicit errors. */
std::optional<std::pair<Quality, Format>> parse_quality_format(std::string_view token) {
    const auto dot = token.find('.');
    if (dot == std::string_view::npos) { return std::nullopt; }
    const auto quality_name = token.substr(0, dot);
    const auto format_name = token.substr(dot + 1);
    std::optional<Quality> quality;
    if (quality_name == "default") { quality = Quality::default_quality; }
    else if (quality_name == "color") { quality = Quality::color; }
    else if (quality_name == "gray") { quality = Quality::gray; }
    else if (quality_name == "bitonal") { quality = Quality::bitonal; }
    std::optional<Format> format;
    if (format_name == "jpg") { format = Format::jpg; }
    else if (format_name == "tif") { format = Format::tif; }
    else if (format_name == "png") { format = Format::png; }
    else if (format_name == "gif") { format = Format::gif; }
    else if (format_name == "jp2") { format = Format::jp2; }
    else if (format_name == "pdf") { format = Format::pdf; }
    else if (format_name == "webp") { format = Format::webp; }
    if (!quality || !format) { return std::nullopt; }
    return std::pair{*quality, *format};
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') { return c - '0'; }
    if (c >= 'a' && c <= 'f') { return c - 'a' + 10; }
    if (c >= 'A' && c <= 'F') { return c - 'A' + 10; }
    return -1;
}

/** Decode one segment, never recursively. Path '+' is not form-encoded space. */
std::optional<std::string> decode_segment(std::string_view segment) {
    std::string decoded;
    decoded.reserve(segment.size());
    for (std::size_t i = 0; i < segment.size(); ++i) {
        auto c = static_cast<unsigned char>(segment[i]);
        if (c == '%') {
            if (segment.size() - i < 3) { return std::nullopt; }
            const int high = hex_digit(segment[i + 1]), low = hex_digit(segment[i + 2]);
            if (high < 0 || low < 0) { return std::nullopt; }
            c = static_cast<unsigned char>(high * 16 + low);
            i += 2;
        }
        if (c < 0x20 || c == 0x7f) { return std::nullopt; }
        decoded.push_back(static_cast<char>(c));
    }
    return decoded;
}

} // namespace

ParseResult parse_path(std::string_view encoded_path) {
    if (!encoded_path.starts_with('/') || encoded_path.find_first_of("?#") != std::string_view::npos) {
        return ParseError{Component::path, "Expected an absolute URL path without query or fragment"};
    }
    encoded_path.remove_prefix(1);
    std::vector<std::string> segments;
    while (true) {
        const auto slash = encoded_path.find('/');
        const auto segment = encoded_path.substr(0, slash);
        if (segment.empty()) {
            return ParseError{Component::path, "Empty path segments are not supported"};
        }
        auto decoded = decode_segment(segment);
        if (!decoded) {
            return ParseError{Component::path, "Malformed percent escape or control character in path"};
        }
        segments.push_back(std::move(*decoded));
        if (slash == std::string_view::npos) { break; }
        encoded_path.remove_prefix(slash + 1);
    }

    const bool info = segments.back() == "info.json";
    const std::size_t suffix_count = info ? 1 : 4;
    if (segments.size() < suffix_count + 1) {
        return ParseError{Component::path, "Expected identifier followed by info.json or four image parameters"};
    }
    const auto identifier_index = segments.size() - suffix_count - 1;
    // Clients normalize dot segments, so they cannot identify a stable service.
    // Leave parameter diagnostics (e.g. rotation='.') to their specific parsers.
    for (std::size_t i = 0; i <= identifier_index; ++i) {
        if (segments[i] == "." || segments[i] == "..") {
            return ParseError{Component::path, "Dot path segments are not supported"};
        }
    }
    Resource resource;
    resource.prefix.reserve(identifier_index);
    for (std::size_t i = 0; i < identifier_index; ++i) {
        resource.prefix.push_back(std::move(segments[i]));
    }
    resource.identifier = std::move(segments[identifier_index]);
    if (info) { return InfoRequest{std::move(resource)}; }

    const auto region = parse_region(segments[identifier_index + 1]);
    if (!region) { return ParseError{Component::region, "Invalid region: expected full, square, x,y,w,h, or pct:x,y,w,h with positive dimensions"}; }
    const auto size = parse_size(segments[identifier_index + 2]);
    if (!size) { return ParseError{Component::size, "Invalid size: expected max, w,, ,h, w,h, !w,h, or pct:n, optionally prefixed by ^; dimensions must be positive"}; }
    const auto rotation = parse_rotation(segments[identifier_index + 3]);
    if (!rotation) { return ParseError{Component::rotation, "Invalid rotation: expected a decimal in [0, 360], optionally prefixed by !"}; }
    const auto quality_format = parse_quality_format(segments[identifier_index + 4]);
    if (!quality_format) { return ParseError{Component::quality_format, "Invalid or unsupported quality.format"}; }
    return ImageRequest{std::move(resource), *region, *size, *rotation,
                        quality_format->first, quality_format->second};
}

} // namespace sipi::iiif
