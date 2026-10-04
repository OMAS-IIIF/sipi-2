#include "cache/CacheKey.h"
#include <cmath>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <locale>
#include <stdexcept>
#include <vector>

namespace {
using namespace sipi;
using namespace sipi::cache;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
RenderIdentity base() {
    return {{"local:1", "v1", 1234}, "original", {{0,0,1000,800},500,400},
        {false,0}, iiif::Quality::default_quality, PixelLayout::rgb, JpegEncoding{90}, std::nullopt};
}

/** Follow parser -> geometry -> identity, using the SAME opened source. URL
 * aliases are deliberately absent only AFTER their source/policy is resolved. */
CacheKey from_url(std::string_view url, PixelLayout layout = PixelLayout::rgb) {
    auto request = std::get<iiif::ImageRequest>(iiif::parse_path(url));
    auto input = base();
    input.geometry = std::get<iiif::ImageGeometry>(iiif::resolve_geometry(
        request.region,request.size,1000,800));
    input.rotation = request.rotation;
    input.quality = request.quality;
    input.input_layout = layout;
    if (request.format == iiif::Format::png) input.encoding = PngEncoding{};
    else require(request.format == iiif::Format::jpg,"test URL output format");
    return CacheKey(input);
}

struct Punctuation : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '_'; }
    std::string do_grouping() const override { return "\3"; }
};
struct LocaleGuard {
    std::locale old = std::locale();
    ~LocaleGuard() { std::locale::global(old); }
};
} // namespace

int main() {
    try {
        const CacheKey original(base());
        // Frozen v1 wire format and independently calculated Python hashlib digest.
        // Changes require a deliberate schema/recipe decision, not a new snapshot.
        constexpr std::string_view expected = R"(sipi-result-key-v1
source.identity:7:local:1
source.version:2:v1
source.bytes:4:1234
variant:8:original
watermark.present:1:0
crop.x:1:0
crop.y:1:0
crop.width:4:1000
crop.height:3:800
size.width:3:500
size.height:3:400
mirror:1:0
rotation.binary64:16:0000000000000000
quality:5:color
encoding:4:jpeg
jpeg.quality:2:90
recipe:14:sipi-render-v1
)";
        require(original.serialized()==expected,"stable v1 serialization");
        require(original.digest()=="24fcc9c00204d2616c166bebf6d45123f887c0e63b62805600362cf2e58fca86","stable SHA-256 digest");
        require(original.digest().size()==64 && original.digest().find_first_not_of("0123456789abcdef")==std::string_view::npos,
                "digest is a filename-safe lowercase SHA-256");

        for (const auto* url : {
            "/id/full/500,/0/default.jpg",
            "/alias/project/id/0,0,1000,800/500,400/360/color.jpg",
            "/another/prefix/other-id/pct:0,0,100,100/pct:50/0.000/default.jpg",
            "/id/full/,400/0/color.jpg", "/id/full/!500,400/0/default.jpg",
            "/id/full/^500,/0/default.jpg"})
            require(from_url(url)==original,"equivalent syntax/aliases after source resolution");
        require(from_url("/id/square/200,/0/default.png") ==
                from_url("/id/100,0,800,800/200,200/0/color.png"),"square resolves before keying");
        require(from_url("/id/900,700,500,500/max/0/default.jpg") ==
                from_url("/id/900,700,100,100/100,100/360/color.jpg"),"clipped geometry equivalence");
        const auto gray = from_url("/id/full/500,/0/default.jpg",PixelLayout::gray);
        require(gray == from_url("/id/full/500,/0/color.jpg",PixelLayout::gray) &&
                gray == from_url("/id/full/500,/0/gray.jpg",PixelLayout::gray),"native gray quality equivalence");
        require(gray != original,"effective gray and color differ");
        auto negative_zero = base(); negative_zero.rotation.degrees = -0.0;
        require(CacheKey(negative_zero)==original,"negative zero angle normalized");
        require(from_url("/id/full/500,/90.0/default.jpg") ==
                from_url("/id/full/500,/90/default.jpg"),"equivalent decimal angles");

        const std::vector<std::function<void(RenderIdentity&)>> changes{
            [](auto& v){v.source.identity="s3:another-source";},
            [](auto& v){v.source.version="v2";}, [](auto& v){++v.source.size;},
            [](auto& v){v.variant="restricted:virtual-1000x800";},
            [](auto& v){++v.geometry.region.x;}, [](auto& v){++v.geometry.region.y;},
            [](auto& v){--v.geometry.region.width;}, [](auto& v){--v.geometry.region.height;},
            [](auto& v){++v.geometry.output_width;}, [](auto& v){++v.geometry.output_height;},
            [](auto& v){v.rotation.mirrored=true;}, [](auto& v){v.rotation.degrees=90;},
            [](auto& v){v.rotation.degrees=std::nextafter(0.0,1.0);},
            [](auto& v){v.quality=iiif::Quality::gray;}, [](auto& v){v.quality=iiif::Quality::bitonal;},
            [](auto& v){v.encoding=JpegEncoding{91};}, [](auto& v){v.encoding=PngEncoding{};},
            [](auto& v){v.recipe="sipi-render-v2-linear-light";},
            [](auto& v){v.watermark=SourceInfo{"local:watermark","w1",99};}
        };
        for (const auto& change : changes) {
            auto value=base(); change(value); const CacheKey key(value);
            require(key!=original && key.digest()!=original.digest(),"output-affecting change separates keys");
        }
        auto marked=base(); marked.variant="restricted:watermark-center";
        marked.watermark=SourceInfo{"local:watermark","w1",99};
        const CacheKey watermark_key(marked);
        for (const auto& change : std::vector<std::function<void(RenderIdentity&)>>{
                [](auto& v){v.watermark->identity="local:other-watermark";},
                [](auto& v){v.watermark->version="w2";}, [](auto& v){++v.watermark->size;},
                [](auto& v){v.variant="restricted:watermark-corner";}, [](auto& v){v.watermark.reset();}}) {
            auto value=marked; change(value);
            require(CacheKey(value)!=watermark_key,"watermark identity/version/placement/presence separate results");
        }

        auto left=base(),right=base();
        left.source.identity="a:b"; left.source.version="c";
        right.source.identity="a"; right.source.version="b:c";
        require(CacheKey(left)!=CacheKey(right),"delimiter ambiguity rejected by framing");
        left.source.identity=std::string("a\0b",3); right.source.identity="a";
        left.source.version=right.source.version="v\nsource.bytes:1:0";
        require(CacheKey(left)!=CacheKey(right),"NUL/newline cannot truncate or inject fields");
        left.variant="museum:ä";
        const CacheKey unicode(left);
        require(unicode.serialized().find("variant:9:museum:ä\n")!=std::string_view::npos,"length counts UTF-8 bytes");
        const std::string saved(unicode.serialized()); left.variant="changed";
        require(unicode.serialized()==saved,"key owns its representation");
        {
            LocaleGuard guard;
            std::locale::global(std::locale(std::locale::classic(),new Punctuation));
            require(CacheKey(base())==original,"locale-independent key bytes");
        }
        std::vector<std::future<CacheKey>> jobs;
        for (unsigned i=0;i<8;++i) jobs.push_back(std::async(std::launch::async,[]{return CacheKey(base());}));
        for (auto& job:jobs) require(job.get()==original,"deterministic concurrent hashing");

        const std::vector<std::function<void(RenderIdentity&)>> invalid{
            [](auto& v){v.source.identity.clear();}, [](auto& v){v.source.version.clear();},
            [](auto& v){v.source.size=0;}, [](auto& v){v.variant.clear();}, [](auto& v){v.recipe.clear();},
            [](auto& v){v.geometry.region.width=0;}, [](auto& v){v.geometry.region.height=0;},
            [](auto& v){v.geometry.region.x=UINT32_MAX;}, [](auto& v){v.geometry.region.y=UINT32_MAX;},
            [](auto& v){v.geometry.output_width=0;}, [](auto& v){v.geometry.output_height=0;},
            [](auto& v){v.rotation.degrees=-1;}, [](auto& v){v.rotation.degrees=361;},
            [](auto& v){v.rotation.degrees=std::numeric_limits<double>::infinity();},
            [](auto& v){v.rotation.degrees=std::numeric_limits<double>::quiet_NaN();},
            [](auto& v){v.quality=static_cast<iiif::Quality>(99);},
            [](auto& v){v.input_layout=static_cast<PixelLayout>(99);},
            [](auto& v){v.encoding=JpegEncoding{0};}, [](auto& v){v.encoding=JpegEncoding{101};},
            [](auto& v){v.watermark=SourceInfo{"","v1",1};},
            [](auto& v){v.watermark=SourceInfo{"id","",1};}, [](auto& v){v.watermark=SourceInfo{"id","v1",0};}
        };
        for (const auto& change:invalid) {
            auto value=base(); change(value); bool caught=false;
            try { (void)CacheKey(value); } catch (const std::invalid_argument&) {caught=true;}
            require(caught,"incomplete/invalid identity is rejected");
        }
        std::cout << "Cache identity normalization, separation, serialization and hashing passed\n";
    } catch(const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
