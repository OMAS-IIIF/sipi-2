#include "imgformats/PngReader.h"
#include "cache/MetadataCache.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>

namespace {
using namespace sipi;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
struct Memory final : ByteSource {
    std::vector<std::byte> bytes;
    SourceInfo facts{"memory:png", "v1", 0};
    mutable std::size_t reads = 0;
    bool failing = false;
    explicit Memory(const std::filesystem::path& file) {
        std::ifstream input(file, std::ios::binary);
        check(bool(input), "missing fixture");
        const std::vector<char> data{std::istreambuf_iterator<char>(input), {}};
        bytes.resize(data.size());
        std::transform(data.begin(), data.end(), bytes.begin(), [](char b) { return std::byte(b); });
        facts.size = bytes.size();
    }
    const SourceInfo& info() const noexcept override { return facts; }
    std::size_t read_at(std::uint64_t offset, std::span<std::byte> target) const override {
        if (failing) throw ByteSourceError(ByteSourceErrorCode::changed, "changed");
        if (offset >= bytes.size()) return 0;
        const auto count = std::min(target.size(), bytes.size() - std::size_t(offset));
        std::copy_n(bytes.data() + offset, count, target.data());
        reads += count;
        return count;
    }
};
template<class Error, class Function> void rejects(Function action) {
    bool failed = false;
    try { action(); } catch (const Error&) { failed = true; }
    check(failed, "expected reader failure");
}
void run(const std::filesystem::path& root) {
    struct Case { const char* name; PngColor color; unsigned bits; bool alpha; bool interlaced; };
    for (const auto& test : {
        Case{"rgb",PngColor::rgb,8,false,false}, {"gray",PngColor::gray,8,false,false},
        {"rgba",PngColor::rgba,8,true,false}, {"gray-alpha",PngColor::gray_alpha,8,true,false},
        {"palette",PngColor::palette,2,false,false}, {"palette-alpha",PngColor::palette,2,true,false},
        {"gray-low",PngColor::gray,1,false,false}, {"rgb-trns",PngColor::rgb,8,true,false},
        {"gray-trns",PngColor::gray,8,true,false}, {"adam7",PngColor::rgb,8,false,true}}) {
        Memory source(root / (std::string(test.name)+".png"));
        const auto info = read_png_metadata(source);
        check(info.width == 3 && info.height == 2, "PNG dimensions");
        check(info.color == test.color && info.bit_depth == test.bits, "PNG layout");
        check(info.transparency == test.alpha && info.interlaced == test.interlaced, "PNG flags");
    }
    Memory tagged(root / "icc.png");
    const auto metadata = read_png_metadata(tagged);
    Memory expected(root / "expected.icc");
    check(metadata.icc_profile.size() == expected.bytes.size(), "ICC size");
    check(std::equal(metadata.icc_profile.begin(), metadata.icc_profile.end(), expected.bytes.begin(),
        [](auto a, auto b) { return std::byte(a) == b; }), "ICC bytes");
    for (const auto* name : {"sixteen.png", "exif.png", "cicp.png"})
        rejects<PngUnsupportedError>([&] { (void)read_png_metadata(Memory(root/name)); });
    for (const auto* name : {"broken.png", "bad-crc.png", "bad-icc.png", "bad-palette.png", "zero.png"})
        rejects<PngReadError>([&] { (void)read_png_metadata(Memory(root/name)); });
    rejects<PngReadError>([&] { (void)read_png_metadata(tagged, 20); });
    for (const auto* name : {"rgb","gray","rgba","gray-alpha","palette","palette-alpha",
             "gray-low","rgb-trns","gray-trns","adam7","adam7-large","icc","gray-icc","gray-alpha-icc","unknown",
             "gray-two","gray-four","gray-low-trns","palette-adam7","rgba-adam7",
             "srgb-gamma","srgb","srgb-1","srgb-2","srgb-3","srgb-gray","srgb-gray-alpha","srgb-rgba","srgb-palette"}) {
        Memory input(root/(std::string(name)+".png"));
        const auto facts = read_png_metadata(input);
        const auto raster = read_png_image(input,facts);
        Memory reference(root/(std::string(name)+".raw"));
        check(raster.width == facts.width && raster.height == facts.height, "decoded dimensions");
        check(raster.pixels.size() == reference.bytes.size(), "decoded size");
        check(std::equal(raster.pixels.begin(), raster.pixels.end(), reference.bytes.begin(),
            [](auto a, auto b) { return std::byte(a) == b; }), "decoded PNG pixel mismatch");
        check(raster.icc_profile == facts.icc_profile, "decoded ICC");
        check(raster.srgb_intent == facts.srgb_intent, "decoded sRGB intent");
        if (std::string(name).starts_with("srgb"))
            check(facts.srgb_intent.has_value() && raster.icc_profile.empty(), "explicit sRGB declaration");
    }
    for (const auto* name : {"header-only","bad-idat","no-iend"}) {
        Memory input(root/(std::string(name)+".png"));
        const auto facts = read_png_metadata(input);
        rejects<PngReadError>([&] { (void)read_png_image(input,facts); });
    }
    for (const auto* name : {"gamma","late-exif","late-apng"}) {
        Memory input(root/(std::string(name)+".png"));
        const auto facts = read_png_metadata(input);
        rejects<PngUnsupportedError>([&] { (void)read_png_image(input,facts); });
    }
    rejects<PngUnsupportedError>([&] { (void)read_png_metadata(Memory(root/"apng.png")); });
    rejects<PngReadError>([&] { (void)read_png_metadata(Memory(root/"critical.png")); });
    rejects<PngReadError>([&] { (void)read_png_image(tagged,metadata,17); });
    rejects<PngReadError>([&] { (void)read_png_image(tagged,metadata,18+metadata.icc_profile.size()-1); });
    rejects<PngReadError>([&] { (void)read_png_image(tagged,metadata,1000000,tagged.bytes.size()-1); });
    auto wrong = metadata; ++wrong.width;
    rejects<PngReadError>([&] { (void)read_png_image(tagged,wrong); });
    tagged.failing = true;
    rejects<ByteSourceError>([&] { (void)read_png_image(tagged,metadata); });
    rejects<ByteSourceError>([&] { (void)read_png_metadata(tagged); });
    // Header-only success says nothing about IDAT pixels/CRC or IEND.
    (void)read_png_metadata(Memory(root / "header-only.png"));
    Memory large(root / "rgb.png"); large.bytes.resize(1024*1024); large.facts.size = large.bytes.size();
    (void)read_png_metadata(large);
    check(large.reads <= 16384, "PNG header staged the whole source");
    std::vector<std::future<void>> tasks;
    for (unsigned i=0; i<8; ++i) tasks.push_back(std::async(std::launch::async, [&] {
        check(read_png_image(Memory(root/"icc.png"), metadata).icc_profile == metadata.icc_profile, "parallel PNG decode");
    }));
    for (auto& task : tasks) task.get();
    cache::MetadataCache cache;
    auto snapshot = std::make_shared<const cache::ImageMetadata>(metadata);
    check(cache.put(tagged.info(), snapshot), "PNG cache admission");
    check(std::get<PngMetadata>(*cache.get(tagged.info())).icc_profile == metadata.icc_profile, "cached ICC");
    check(cache.resident_bytes() >= metadata.icc_profile.size(), "PNG ICC accounting");
    check(!cache::MetadataCache({10,100}).put(tagged.info(),snapshot), "PNG cache budget");
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try { run(argv[1]); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "PNG metadata, limits, storage and cache checks passed\n";
}
