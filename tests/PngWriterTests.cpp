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
void save(const std::filesystem::path& path, const sipi::RasterImage8& image) {
    std::ofstream output(path, std::ios::binary);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    sipi::write_png(image, [&](auto bytes) {
        check(!bytes.empty() && bytes.size() <= 16384, "Unbounded sink block");
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    });
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
        const sipi::PngSink discard = [](auto) {};
        rejects([&] { sipi::write_png(image, {}); });
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
        std::cout << "PNG validation, ownership, bounded callbacks and failure recovery passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
