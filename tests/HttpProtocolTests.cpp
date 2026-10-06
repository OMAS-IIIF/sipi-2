#include "http/HttpProtocol.h"
#include <iostream>

namespace {
using namespace sipi::http;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void run() {
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
