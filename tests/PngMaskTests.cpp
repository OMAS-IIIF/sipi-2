#include "imgformats/PngReader.h"
#include "image/Watermark.h"
#include "storage/LocalFileSource.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>

namespace {
using namespace sipi;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class E, class F> void rejects(F action) {
    try { action(); } catch (const E&) { return; }
    throw std::runtime_error("Expected PNG mask failure");
}
struct FailedSource final : ByteSource {
    SourceInfo facts{"test:mask","v1",100};
    const SourceInfo& info() const noexcept override { return facts; }
    std::size_t read_at(std::uint64_t, std::span<std::byte>) const override {
        throw ByteSourceError(ByteSourceErrorCode::changed,"source changed");
    }
};
void run(const std::filesystem::path& root) {
    for (unsigned bits : {1,2,4,8}) for (unsigned interlace : {0,1}) {
        const auto name="gray-"+std::to_string(bits)+"-"+std::to_string(interlace);
        LocalFileSource source(root/(name+".png"));
        const auto mask=read_png_mask(source,187);
        std::ifstream input(root/(name+".raw"),std::ios::binary);
        const std::vector<std::uint8_t> expected{std::istreambuf_iterator<char>(input),{}};
        check(mask.width==17 && mask.height==11 && mask.pixels==expected,"expanded coverage bytes");
        check(mask.layout==PixelLayout::gray && mask.icc_profile.empty() && !mask.srgb_intent,"mask color declaration");
        auto mark=prepare_watermark(mask,17,11,1);
        RasterImage8 black{17,11,std::vector<std::uint8_t>(187),{},PixelLayout::gray};
        const std::array<std::uint8_t,1> white{255};
        check(apply_watermark(std::move(black),mark,white,1).pixels==expected,"PNG-to-compositor contract");
        rejects<PngReadError>([&]{ (void)read_png_mask(source,186); });
        rejects<PngReadError>([&]{ (void)read_png_mask(source,187,0); });
        rejects<PngReadError>([&]{ (void)read_png_mask(source,187,20); });
        (void)read_png_mask(source,187,source.info().size);
        rejects<PngReadError>([&]{ (void)read_png_mask(source,187,source.info().size-1); });
    }
    for (const auto* name : {"rgb","rgba","gray-alpha","palette","sixteen","trns","gamma","srgb","icc","chrm","cicp","apng","late-apng"}) {
        LocalFileSource source(root/(std::string(name)+".png"));
        rejects<PngUnsupportedError>([&]{ (void)read_png_mask(source); });
    }
    // libpng may classify malformed/late metadata as a warning before the
    // explicit subset check. Both paths must fail, never return partial masks.
    for (const auto* name : {"exif","late-gamma","truncated","missing-iend","bad-crc","bad-idat"}) {
        LocalFileSource source(root/(std::string(name)+".png"));
        rejects<PngReadError>([&]{ (void)read_png_mask(source); });
    }
    rejects<ByteSourceError>([]{ (void)read_png_mask(FailedSource{}); });
    LocalFileSource shared(root/"gray-8-1.png");
    auto a=std::async(std::launch::async,[&]{return read_png_mask(shared).pixels;});
    auto b=std::async(std::launch::async,[&]{return read_png_mask(shared).pixels;});
    check(a.get()==b.get(),"concurrent mask decode");
}
}
int main(int argc, char** argv) {
    try { check(argc==2,"fixture directory required"); run(argv[1]); std::cout<<"PNG mask tests passed\n"; }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
