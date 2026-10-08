#pragma once

#include <json/json.h>

#include <cstdint>
#include <optional>
#include <string>

namespace sipi::preflight {
/** Operator-selected algorithm/key. HS256 uses raw secret bytes (>=32); RS256
 * uses an unencrypted private PEM for signing and public PEM for verification.
 * Keys are never inferred from token headers or fetched over the network. */
struct JwtOptions {
    std::string algorithm;
    std::string key;
    std::optional<std::string> issuer;
    std::optional<std::string> audience;
    std::optional<std::string> type;
    std::optional<std::string> kid;
    std::optional<std::int64_t> expires_in;
    std::int64_t leeway = 0;
    bool require_exp = true;
};
struct JwtFailure {
    const char* code;
    const char* message;
};
struct JwtResult {
    Json::Value claims;
    std::optional<JwtFailure> failure;
};

/** Sign/verify compact JWS JWTs with explicit policy and bounded token/key sizes.
 * now is UTC Unix seconds, injected for deterministic boundary tests. Functions
 * own temporary crypto objects, have no global mutable state and perform no I/O.
 * Invalid operator options/signing claims throw; untrusted token failures return
 * credential-free diagnostics and never expose unverified claims. */
std::string sign_jwt(Json::Value claims, const JwtOptions& options, std::int64_t now);
JwtResult verify_jwt(const std::string& token, const JwtOptions& options, std::int64_t now);
}
