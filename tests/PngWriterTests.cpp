#include "imgformats/PngWriter.h"
#include "IccFixture.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
template<class F> void rejects(F action) {
    try { action(); } catch (const sipi::PngWriteError&) { return; }
    throw std::runtime_error("Expected PNG failure");
}
void save(const std::filesystem::path& path, const sipi::RasterImage8& image,
          sipi::PngBitDepth depth = sipi::PngBitDepth::eight) {
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    sipi::write_png(image, [&](auto bytes) {
        check(!bytes.empty() && bytes.size() <= 16384, "Unbounded sink block");
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }, depth);
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        const std::filesystem::path root{argv[1]};
        sipi::RasterImage8 image{257, 129, std::vector<std::uint8_t>(257*129*3), {}};
        std::uint32_t random = 17;
        for (auto& byte : image.pixels) { random = random*1664525u + 1013904223u; byte = random >> 24; }
        save(root / "untagged.png", image);
        image.icc_profile = fixture::icc_header();
        save(root / "profile.png", image);
        save(root / "one.png", {1, 1, {0, 127, 255}, {}});
        save(root / "column.png", {1, 3, {1,2,3,4,5,6,7,8,9}, {}});
        save(root / "row.png", {3, 1, {1,2,3,4,5,6,7,8,9}, {}});
        save(root / "rgba.png", {2,1,{255,0,0,128,0,0,255,0},fixture::icc_header(),sipi::PixelLayout::rgba});
        auto gray_profile = fixture::icc_header();
        std::copy_n("GRAY",4,gray_profile.begin()+16);
        save(root / "gray.png", {3,1,{0,127,255},gray_profile,sipi::PixelLayout::gray});
        for (const unsigned width : {1,7,8,9,17,257}) {
            sipi::RasterImage8 binary{width,5,std::vector<std::uint8_t>(width*5),
                gray_profile,sipi::PixelLayout::gray};
            for (unsigned y=0; y<5; ++y)
                for (unsigned x=0; x<width; ++x)
                    binary.pixels[y*width+x] = (x+2*y)%3==0 ? 255 : 0;
            save(root / ("bitonal-"+std::to_string(width)+".png"),binary,sipi::PngBitDepth::one);
        }
        sipi::RasterImage8 binary{3,1,{0,255,0},{},sipi::PixelLayout::gray,2};
        save(root / "bitonal-srgb.png",binary,sipi::PngBitDepth::one);
        save(root / "binary-eight.png",binary);
        const sipi::PngSink discard = [](auto) {};
        rejects([&] { sipi::write_png(image, {}); });
        std::size_t invalid_calls=0;
        const sipi::PngSink must_not_emit=[&](auto) { ++invalid_calls; };
        rejects([&] { sipi::write_png(image,must_not_emit,sipi::PngBitDepth::one); });
        rejects([&] { sipi::write_png({1,1,{127},{},sipi::PixelLayout::gray},must_not_emit,sipi::PngBitDepth::one); });
        rejects([&] { sipi::write_png({1,1,{0,255},{},sipi::PixelLayout::gray_alpha},must_not_emit,sipi::PngBitDepth::one); });
        rejects([&] { sipi::write_png(binary,must_not_emit,static_cast<sipi::PngBitDepth>(99)); });
        check(invalid_calls==0,"Invalid one-bit data emitted bytes");
        auto conflicting = image; conflicting.srgb_intent = 0;
        rejects([&] { sipi::write_png(conflicting, discard); });
        conflicting.icc_profile.clear(); conflicting.srgb_intent = 4;
        rejects([&] { sipi::write_png(conflicting, discard); });
        rejects([&] { sipi::write_png({1,1,{0},fixture::icc_header(),sipi::PixelLayout::gray}, discard); });
        rejects([&] { sipi::write_png({1,1,{0,0,0},gray_profile}, discard); });
        rejects([&] { sipi::write_png({1,1,{1,2,3},{},sipi::PixelLayout::rgba}, discard); });
        rejects([&] { sipi::write_png({1,1,{1,2,3},{},static_cast<sipi::PixelLayout>(99)}, discard); });
        rejects([&] { sipi::write_png({0,1,{},{}}, discard); });
        rejects([&] { sipi::write_png({1,1,{1},{}}, discard); });
        rejects([&] { sipi::write_png({UINT32_MAX,UINT32_MAX,{},{}}, discard); });
        rejects([&] { sipi::write_png({1000001,1,{},{}}, discard); });
        rejects([&] { sipi::write_png({1,1,{0,0,0}, {1,2,3}}, discard); });
        auto bad = fixture::icc_header(); bad[16] = 'X';
        rejects([&] { sipi::write_png({1,1,{0,0,0}, bad}, discard); });
        bad = fixture::icc_header(); bad[3] = 136; // libpng error after setjmp
        rejects([&] { sipi::write_png({1,1,{0,0,0}, bad}, discard); });
        rejects([&] { sipi::write_png({1,1,{0,0,0}, std::vector<std::uint8_t>(16*1024*1024+1)}, discard); });
        // Throw before output, midway through IDAT and at the final callback.
        struct SinkFailure {};
        std::size_t calls = 0;
        sipi::write_png(image, [&](auto) { ++calls; });
        check(calls > 10, "Expected streaming output");
        for (const auto at : {std::size_t{1}, calls/2, calls}) {
            std::size_t seen = 0;
            bool caught = false;
            try { sipi::write_png(image, [&](auto) { if (++seen == at) throw SinkFailure{}; }); }
            catch (const SinkFailure&) { caught = true; }
            check(caught && seen == at, "Sink exception lost or output continued");
            sipi::write_png(image, discard);
        }
        calls=0;
        sipi::write_png(binary,[&](auto) { ++calls; },sipi::PngBitDepth::one);
        for (const auto at : {std::size_t{1}, calls/2, calls}) {
            std::size_t seen=0;
            bool caught=false;
            try {
                sipi::write_png(binary,[&](auto) { if (++seen==at) throw SinkFailure{}; },sipi::PngBitDepth::one);
            } catch (const SinkFailure&) { caught=true; }
            check(caught && seen==at,"One-bit sink exception lost");
            sipi::write_png(binary,discard,sipi::PngBitDepth::one);
        }
        std::cout << "PNG validation, ownership, bounded callbacks and failure recovery passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
