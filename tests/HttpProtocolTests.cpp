#include "http/HttpProtocol.h"
#include <iostream>

namespace {
using namespace sipi::http;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void run() {
    const auto parsed = std::get<QueryParameters>(parse_query_parameters(
        "token=first&%74oken=second&space=a+b&plus=%2B&once=%252F&flag&empty=&x=a=b%26c;d"));
    check(parsed.at("token") == std::vector<std::string>{"first", "second"}, "duplicate decoded names");
    check(parsed.at("space")[0] == "a b" && parsed.at("plus")[0] == "+", "query plus decoding");
    check(parsed.at("once")[0] == "%2F", "decode exactly once");
    check(parsed.at("flag")[0].empty() && parsed.at("empty")[0].empty(), "present empty values");
    check(parsed.at("x")[0] == "a=b&c;d", "split before decode, only ampersand separator");
    check(std::get<QueryParameters>(parse_query_parameters("")).empty(), "absent query");
    for (const auto raw : {"=x", "&x=y", "x=y&", "x=y&&z=w", "x=%", "x=%0", "x=%GG",
                           "x=%00", "x=%0d", "x=%0A", "x=%7F", "x=#fragment", "x=\n"}) {
        check(parse_query_parameters(raw) == std::variant<QueryParameters, QueryError>{QueryError::malformed},
              "malformed query rejected");
    }
    check(std::get<QueryParameters>(parse_query_parameters("x=%23")).at("x")[0] == "#", "encoded hash allowed");
    for (const auto& raw : {std::string(257, 'n') + "=x", "x=" + std::string(16385, 'v'),
                            std::string(32769, 'x')}) {
        check(parse_query_parameters(raw) == std::variant<QueryParameters, QueryError>{QueryError::limit},
              "query byte limits");
    }
    check(std::holds_alternative<QueryParameters>(parse_query_parameters(std::string(256, 'n') + "=" +
                                                                  std::string(16384, 'v'))), "exact byte limits");
    std::string pairs = "x=1";
    for (int i = 1; i < 128; ++i) pairs += "&x=1";
    check(std::get<QueryParameters>(parse_query_parameters(pairs)).at("x").size() == 128, "exact pair limit");
    check(parse_query_parameters(pairs + "&x=1") == std::variant<QueryParameters, QueryError>{QueryError::limit},
          "pair limit includes repeated names");
    using enum InfoMediaType;
    check(negotiate_info_type({})==json_ld,"absent Accept");
    check(negotiate_info_type("application/json")==json,"JSON");
    check(negotiate_info_type("application/ld+json")==json_ld,"JSON-LD");
    check(negotiate_info_type("application/json;q=0.8,application/ld+json;q=0.2")==json,"quality preference");
    check(negotiate_info_type("application/ld+json;q=0,*/*;q=1")==json,"specific exclusion");
    check(!negotiate_info_type("application/ld+json;q=0,application/json;q=0,*/*"),"exclude both");
    check(negotiate_info_type("Application/JSON; Charset=\"UTF-8\"")==json,"case and charset");
    check(negotiate_info_type("application/ld+json;profile=\"http://iiif.io/api/image/3/context.json\"")==json_ld,"profile");
    check(!negotiate_info_type("application/ld+json;profile=\"other\""),"unknown profile");
    check(negotiate_info_type("application/json;q=1;extension=\"x,y;z\"")==json,"quoted extension");
    check(!negotiate_info_type("application/json;q=1.001"),"q range");
    check(!negotiate_info_type("application/json;q=0.1234"),"q precision");
    check(!negotiate_info_type("application/json;q=-1"),"negative q");
    check(!negotiate_info_type("application/json;q=\"1\""),"quoted q invalid");
    check(!negotiate_info_type("application/json; charset=latin1"),"unsupported charset");
    check(!negotiate_info_type("application/json;q=1;q=0"),"duplicate q");
    check(!negotiate_info_type("application/json;profile=\"unterminated"),"broken quoting");
    check(!negotiate_info_type(""),"empty header");
    check(!negotiate_info_type("text/html"),"unacceptable type");
    check(allowed_cors_preflight("GET","authorization, Accept"),"allowed headers");
    check(!allowed_cors_preflight("POST","Authorization"),"reject write");
    check(!allowed_cors_preflight("GET","X-Secret"),"reject unknown header");
    check(!allowed_cors_preflight("GET","\"broken"),"malformed header list");
    for (const auto path : {"/id","/a/b/c/d/e/id","/a%2Fb/id","/a/b/c/d/color"}) {
        const auto r=route_iiif_path(path);
        check(r.redirect_to_info && std::holds_alternative<sipi::iiif::InfoRequest>(r.query),"base redirect");
    }
    for (const auto path : {"/id/full/max/0/default.jpg","/id/info.json"}) {
        const auto r=route_iiif_path(path);
        check(!r.redirect_to_info && !std::holds_alternative<sipi::iiif::ParseError>(r.query),"normal route");
    }
    for (const auto path : {"/","/id/","/id%GG","/id/no-region/max/0/default.jpg",
                            "/id/full/max/999/default.jpg","/id/full/max/0/invalid.jpg",
                            "/id/%2566ull/max/0/default.jpg"}) {
        const auto r=route_iiif_path(path);
        check(!r.redirect_to_info && std::holds_alternative<sipi::iiif::ParseError>(r.query),"malformed image stays error");
    }
}
}
int main() {
    try { run(); std::cout<<"HTTP routing/negotiation/CORS checks passed\n"; }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
