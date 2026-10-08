#include "preflight/Jwt.h"
#include "preflight/LuaPreflight.h"

#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <vector>

namespace {
using namespace sipi::preflight;
std::string read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("fixture not found");
    return {std::istreambuf_iterator<char>(input), {}};
}
void check(bool ok, const std::string& label) {
    if (!ok) throw std::runtime_error(label);
}
}

/** Independent Python/OpenSSL fixtures exercise real native verification; the
 * injected clock makes exact expiry/leeway and post-2038 cases deterministic. */
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        const std::filesystem::path directory(argv[1]);
        Json::Value cases;
        std::ifstream input(directory / "cases.json");
        input >> cases;
        for (const auto& item : cases) {
            JwtOptions options;
            options.algorithm = item.get("algorithm", "HS256").asString();
            options.key = options.algorithm == "HS256" ? std::string(32, 's') : read(directory / "public.pem");
            if (item.isMember("issuer")) options.issuer = item["issuer"].asString();
            if (item.isMember("audience")) options.audience = item["audience"].asString();
            if (item.isMember("type")) options.type = item["type"].asString();
            options.require_exp = item.get("require_exp", true).asBool();
            options.leeway = item.get("leeway", 0).asInt64();
            const auto result = verify_jwt(item["token"].asString(), options, item.get("now", 1800000000).asInt64());
            const auto expected = item["error"].asString();
            check(expected.empty() ? !result.failure : result.failure && result.failure->code == expected,
                  "verification case: " + item["name"].asString());
            if (result.failure) check(result.claims.isNull(), "no unverified claims on failure");
            else if (item.isMember("claims")) check(result.claims == item["claims"], "exact nested claims round trip");
        }
        for (const auto* algorithm : {"HS256", "RS256"}) {
            JwtOptions options;
            options.algorithm = algorithm;
            options.key = options.algorithm == "HS256" ? std::string(32, 's') : read(directory / "private.pem");
            options.expires_in = 300;
            options.kid = "test-key";
            Json::Value claims(Json::objectValue);
            claims["iss"] = "test-issuer";
            claims["aud"] = "test-audience";
            claims["nested"]["items"].append(Json::Value{});
            claims["nested"]["items"].append(false);
            claims["large"] = Json::Int64(9223372036854775807LL);
            const auto token = sign_jwt(claims, options, 2200000000LL); // Beyond 2038.
            std::ofstream(directory / (std::string(algorithm) + ".jwt")) << token;
            if (options.algorithm == "RS256") options.key = read(directory / "public.pem");
            options.issuer = "test-issuer";
            options.audience = "test-audience";
            const auto result = verify_jwt(token, options, 2200000000LL);
            check(!result.failure && result.claims["exp"].asInt64() == 2200000300LL &&
                  result.claims["large"].asInt64() == 9223372036854775807LL, "post-2038 sign/verify");
        }
        for (const auto* name : {"public.pem", "weak-public.pem", "ec-public.pem", "private.pem"}) {
            JwtOptions options;
            options.algorithm = std::string(name) == "public.pem" ? "HS256" : "RS256";
            options.key = read(directory / name);
            bool rejected = false;
            try { (void)verify_jwt("invalid", options, 1800000000); }
            catch (const std::invalid_argument&) { rejected = true; }
            check(rejected, "wrong/weak key rejected before token processing");
        }
        for (const auto& path : {"script.lua", "budget.lua", "pressure.lua"}) {
            const LuaPreflight script(directory / path);
            script.validate();
            std::vector<std::future<Decision>> futures;
            for (int i = 0; i < 4; ++i) futures.push_back(std::async(std::launch::async, [&] {
                return script.evaluate({{{}, "example"}, {}, {}});
            }));
            for (auto& future : futures) check(std::holds_alternative<Allow>(future.get()), "Lua JWT contract");
        }
        std::cout << cases.size() << " JWT verification vectors and native/Lua lifecycle tests passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
