/** Offline JPEG decode benchmark. Generates deterministic 6000x4000 RGB JPEGs
 * (quality 90, 4:2:0, baseline/progressive) in a caller-owned output directory.
 * Times current full versus region decoding with warm metadata and file cache.
 * Two warmup pairs precede measured pairs; order alternates to reduce drift.
 * Every decoded result is compared with the full reference outside timing.
 * CSV on stdout retains individual samples. No HTTP, resizing or output encoding.
 */
#include "imgformats/JpegReader.h"
#include "storage/LocalFileSource.h"
#include <cstdio>
#include <jpeglib.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace sipi;
constexpr unsigned width = 6000, height = 4000;
using Clock = std::chrono::steady_clock;
struct Case { const char* name; PixelRegion region; };

/** Same source samples for both codings; no platform-dependent random library. */
void generate(const std::filesystem::path& path, bool progressive) {
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file) throw std::runtime_error("Cannot create benchmark JPEG");
    jpeg_compress_struct codec{};
    jpeg_error_mgr errors{};
    codec.err = jpeg_std_error(&errors);
    jpeg_create_compress(&codec);
    jpeg_stdio_dest(&codec, file);
    codec.image_width = width; codec.image_height = height;
    codec.input_components = 3; codec.in_color_space = JCS_RGB;
    jpeg_set_defaults(&codec);
    codec.comp_info[0].h_samp_factor = codec.comp_info[0].v_samp_factor = 2;
    jpeg_set_quality(&codec, 90, TRUE);
    if (progressive) jpeg_simple_progression(&codec);
    jpeg_start_compress(&codec, TRUE);
    std::vector<JSAMPLE> row(width * 3);
    std::uint32_t noise = 20261123;
    while (codec.next_scanline < height) {
        const auto y = codec.next_scanline;
        for (unsigned x = 0; x < width; ++x) for (unsigned c = 0; c < 3; ++c) {
            noise ^= noise << 13; noise ^= noise >> 17; noise ^= noise << 5;
            row[x * 3 + c] = static_cast<JSAMPLE>((x * (3 + c) + y * (5 - c) + noise % 40) % 256);
        }
        JSAMPROW pointer = row.data();
        jpeg_write_scanlines(&codec, &pointer, 1);
    }
    jpeg_finish_compress(&codec);
    jpeg_destroy_compress(&codec);
    std::fclose(file);
}

/** Verify all samples, dimensions and layout outside the timed interval. */
void verify(const RasterImage8& result, const RasterImage8& full, PixelRegion region) {
    if (result.width != region.width || result.height != region.height || result.layout != full.layout)
        throw std::runtime_error("Unexpected decoded dimensions/layout");
    for (unsigned y = 0; y < region.height; ++y) {
        const auto first = result.pixels.begin() + std::size_t(y) * region.width * 3;
        const auto expected = full.pixels.begin() + (std::size_t(y + region.y) * full.width + region.x) * 3;
        if (!std::equal(first, first + region.width * 3, expected))
            throw std::runtime_error("Region pixels differ from full decode");
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc != 3) throw std::runtime_error("Usage: jpeg-region-benchmark OUTPUT_DIRECTORY REPETITIONS");
        const std::filesystem::path directory(argv[1]);
        const int repetitions = std::stoi(argv[2]);
        if (repetitions < 1 || repetitions > 100) throw std::runtime_error("Repetitions must be 1..100");
        std::filesystem::create_directories(directory);
        const Case cases[]{
            {"full", {0, 0, width, height}},
            {"centre-512", {(width-512)/2, (height-512)/2, 512, 512}},
            {"centre-1024", {(width-1024)/2, (height-1024)/2, 1024, 1024}},
            {"top-left-512", {0, 0, 512, 512}},
            {"band-500", {0, (height-500)/2, width, 500}}
        };
        std::cout << "coding,case,pair,full_ms,region_ms,raster_bytes\n";
        for (const bool progressive : {false, true}) {
            const auto coding = progressive ? "progressive" : "baseline";
            const auto path = directory / (std::string(coding) + ".jpg");
            generate(path, progressive);
            const LocalFileSource source(path);
            const auto metadata = read_jpeg_metadata(source);
            const auto reference = read_jpeg_image(source, metadata);
            for (const auto& test : cases) for (int pair = -2; pair < repetitions; ++pair) {
                double full_ms = 0, region_ms = 0;
                const auto measure = [&](bool region) {
                    const auto start = Clock::now();
                    const auto pixels = region ? read_jpeg_region(source, metadata, test.region).image
                                               : read_jpeg_image(source, metadata);
                    const auto stop = Clock::now();
                    verify(pixels, reference, region ? test.region : PixelRegion{0,0,width,height});
                    return std::chrono::duration<double, std::milli>(stop-start).count();
                };
                if (pair % 2 == 0) { full_ms = measure(false); region_ms = measure(true); }
                else { region_ms = measure(true); full_ms = measure(false); }
                if (pair >= 0) std::cout << coding << ',' << test.name << ',' << pair << ','
                    << full_ms << ',' << region_ms << ',' << std::uint64_t(test.region.width)*test.region.height*3 << '\n';
            }
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
