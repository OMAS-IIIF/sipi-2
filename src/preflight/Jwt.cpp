#include "preflight/Jwt.h"
#include "preflight/LuaJson.h"

#include <jwt-cpp/traits/open-source-parsers-jsoncpp/traits.h>
#include <openssl/pem.h>

#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace sipi::preflight {
namespace {
constexpr std::size_t token_limit = 16384;
void require(bool ok, const char* message) {
    if (!ok) throw std::invalid_argument(message);
}

/** Use jwt-cpp's adapter but retain our strict parser and 64-bit integers.
 * Upstream's JsonCpp adapter declares a 32-bit integer_type; that must not
 * truncate timestamps or identifiers. No JWT date converts to chrono duration. */
struct JsonTraits : jwt::traits::open_source_parsers_jsoncpp {
    using integer_type = std::int64_t;
    static jwt::json::type get_type(const Json::Value& value) {
        if (value.type() == Json::intValue || value.type() == Json::uintValue) return jwt::json::type::integer;
        return open_source_parsers_jsoncpp::get_type(value);
    }
    static integer_type as_integer(const Json::Value& value) {
        if (!value.isInt64()) throw std::bad_cast();
        return value.asInt64();
    }
    static bool parse(Json::Value& value, const std::string& text) {
        value = parse_json(text);
        return true;
    }
    static std::string serialize(const Json::Value& value) { return write_json(value); }
};
using Claim = jwt::basic_claim<JsonTraits>;

/** Callback explicitly refuses password prompting for encrypted PEM keys. */
int no_password(char*, int, int, void*) { return 0; }
void validate_key(const JwtOptions& options, bool signing) {
    require(options.algorithm == "HS256" || options.algorithm == "RS256", "JWT algorithm must be HS256 or RS256");
    require(!options.key.empty() && options.key.size() <= 16384, "JWT key must contain 1..16384 bytes");
    if (options.algorithm == "HS256") {
        require(options.key.size() >= 32, "HS256 requires at least 32 secret bytes");
        require(options.key.find("-----BEGIN") == std::string::npos, "HS256 must not use a PEM key");
        return;
    }
    require(options.key.find('\0') == std::string::npos, "RSA PEM key contains NUL");
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(options.key.data(), static_cast<int>(options.key.size())), BIO_free);
    if (!bio) throw std::bad_alloc();
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(
        signing ? PEM_read_bio_PrivateKey(bio.get(), nullptr, no_password, nullptr)
                : PEM_read_bio_PUBKEY(bio.get(), nullptr, no_password, nullptr), EVP_PKEY_free);
    require(key && EVP_PKEY_is_a(key.get(), "RSA") == 1, "JWT requires a valid RSA PEM key of the appropriate kind");
    require(EVP_PKEY_bits(key.get()) >= 2048 && EVP_PKEY_bits(key.get()) <= 8192, "JWT RSA key must be 2048..8192 bits");
}
void validate_options(const JwtOptions& options, bool signing) {
    validate_key(options, signing);
    require(options.leeway >= 0 && options.leeway <= 300, "JWT leeway must be 0..300 seconds");
    for (const auto* value : {&options.issuer, &options.audience, &options.type, &options.kid})
        if (*value) require(!(*value)->empty() && (*value)->size() <= 1024 && (*value)->find('\0') == std::string::npos,
                            "JWT option string must contain 1..1024 bytes without NUL");
    if (options.expires_in) require(*options.expires_in > 0 && *options.expires_in <= 31536000,
                                    "JWT expires_in must be 1..31536000 seconds");
}

bool valid_claims(const Json::Value& claims) {
    if (!claims.isObject()) return false;
    for (const auto* name : {"exp", "nbf", "iat"}) {
        if (!claims.isMember(name)) continue;
        const auto& value = claims[name];
        if ((value.type() != Json::intValue && value.type() != Json::uintValue) ||
            !value.isInt64() || value.asInt64() < 0) return false;
    }
    for (const auto* name : {"iss", "sub", "jti"})
        if (claims.isMember(name) && !claims[name].isString()) return false;
    if (claims.isMember("aud")) {
        const auto& audience = claims["aud"];
        if (audience.isString()) return !audience.asString().empty();
        if (!audience.isArray() || audience.empty()) return false;
        for (const auto& value : audience) if (!value.isString() || value.asString().empty()) return false;
    }
    return true;
}
JwtResult failed(const char* code, const char* message) { return {{}, JwtFailure{code, message}}; }

/** Require canonical unpadded base64url, including zero unused bits. jwt-cpp
 * supplies the codec; re-encoding rejects alternate signature representations. */
std::string decode_segment(const std::string& segment) {
    require(!segment.empty() && segment.size() % 4 != 1, "Invalid JWT encoding");
    for (unsigned char c : segment)
        require((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_',
                "Invalid JWT encoding");
    auto result = jwt::base::decode<jwt::alphabet::base64url>(jwt::base::pad<jwt::alphabet::base64url>(segment));
    require(jwt::base::trim<jwt::alphabet::base64url>(jwt::base::encode<jwt::alphabet::base64url>(result)) == segment,
            "Invalid JWT encoding");
    return result;
}
}

std::string sign_jwt(Json::Value claims, const JwtOptions& options, std::int64_t now) {
    validate_options(options, true);
    require(now >= 0 && now <= std::numeric_limits<std::int64_t>::max() - 31536000, "Invalid JWT clock");
    require(claims.isObject(), "JWT claims must be an object");
    if (options.expires_in) {
        require(!claims.isMember("exp"), "JWT exp and expires_in cannot both be supplied");
        claims["exp"] = Json::Int64(now + *options.expires_in);
        if (!claims.isMember("iat")) claims["iat"] = Json::Int64(now);
    }
    require(valid_claims(claims), "Invalid JWT registered claim type");
    require(claims.isMember("exp"), "Signing requires exp or expires_in");
    // Bound payload before crypto work; compact token expansion is checked too.
    require(write_json(claims).size() <= 10000, "JWT claims exceed size limit");
    auto builder = jwt::create<JsonTraits>();
    builder.set_type(options.type.value_or("JWT"));
    if (options.kid) builder.set_key_id(*options.kid);
    for (auto it = claims.begin(); it != claims.end(); ++it) builder.set_payload_claim(it.name(), Claim(*it));
    std::string result;
    try {
        if (options.algorithm == "HS256") result = builder.sign(jwt::algorithm::hs256(options.key));
        else result = builder.sign(jwt::algorithm::rs256("", options.key));
    } catch (const std::bad_alloc&) { throw; }
    catch (...) { throw std::runtime_error("JWT signing failed"); }
    require(result.size() <= token_limit, "JWT token exceeds 16 KiB");
    return result;
}

JwtResult verify_jwt(const std::string& token, const JwtOptions& options, std::int64_t now) {
    validate_options(options, false);
    require(now >= 0 && now <= std::numeric_limits<std::int64_t>::max() - 300, "Invalid JWT clock");
    if (token.empty() || token.size() > token_limit) return failed("malformed", "Invalid JWT encoding or size");
    // Keep token parsing separate from trusted key setup so invalid tokens can
    // never downgrade a configuration failure to a normal permission decision.
    std::optional<jwt::decoded_jwt<JsonTraits>> decoded;
    try { decoded.emplace(jwt::decode<JsonTraits>(token, decode_segment)); }
    catch (const std::bad_alloc&) { throw; }
    catch (...) { return failed("malformed", "Invalid JWT encoding or JSON"); }
    const auto& header = decoded->get_header_json();
    const auto& claims = decoded->get_payload_json();
    if (!header.isObject() || !header.isMember("alg") || !header["alg"].isString() ||
        header["alg"].asString() != options.algorithm)
        return failed("algorithm", "JWT algorithm does not match policy");
    for (const auto* name : {"crit", "b64", "jku", "jwk", "x5u", "x5c", "zip", "enc"})
        if (header.isMember(name)) return failed("malformed", "Unsupported JWT header extension");
    for (const auto* name : {"typ", "kid"})
        if (header.isMember(name) && !header[name].isString()) return failed("malformed", "Invalid JWT header type");
    auto verifier = jwt::verify<JsonTraits>();
    // Time checks below use checked int64 seconds and strict exp <= now. This
    // avoids chrono range overflow and the library's inclusive exp boundary.
    for (const auto* name : {"exp", "nbf", "iat"})
        verifier.with_claim(name, [](const auto&, std::error_code&) {});
    try {
        if (options.algorithm == "HS256") verifier.allow_algorithm(jwt::algorithm::hs256(options.key));
        else verifier.allow_algorithm(jwt::algorithm::rs256(options.key, ""));
    } catch (const std::bad_alloc&) { throw; }
    catch (...) { throw std::runtime_error("JWT verification key setup failed"); }
    std::error_code error;
    verifier.verify(*decoded, error);
    if (error) return failed("signature", "JWT signature verification failed");
    if (!valid_claims(claims)) return failed("claims", "Invalid JWT registered claims");
    if (options.require_exp && !claims.isMember("exp")) return failed("claims", "JWT expiration is required");
    if (claims.isMember("exp") && claims["exp"].asInt64() <= now - options.leeway)
        return failed("expired", "JWT has expired");
    for (const auto* name : {"nbf", "iat"})
        if (claims.isMember(name) && claims[name].asInt64() > now + options.leeway)
            return failed("not_yet_valid", "JWT is not yet valid");
    if (options.issuer && (!claims.isMember("iss") || claims["iss"].asString() != *options.issuer))
        return failed("claims", "JWT issuer does not match policy");
    if (options.type && (!header.isMember("typ") || header["typ"].asString() != *options.type))
        return failed("claims", "JWT type does not match policy");
    if (options.audience) {
        const auto& audience = claims["aud"];
        bool match = audience.isString() && audience.asString() == *options.audience;
        if (audience.isArray()) for (const auto& value : audience) match |= value.asString() == *options.audience;
        if (!match) return failed("claims", "JWT audience does not match policy");
    } else if (claims.isMember("aud")) return failed("claims", "JWT audience requires an explicit audience policy");
    return {claims, std::nullopt};
}
}
