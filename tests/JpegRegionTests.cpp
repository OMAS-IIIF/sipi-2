#include "imgformats/JpegReader.h"
#include "storage/LocalFileSource.h"
#include "image/AreaResize.h"

#include <jpeglib.h>

#include <cstdio>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <vector>

/** read_jpeg_region must return exactly the pixels of a full decode for the
 * rectangle it reports, for every sampling layout, progressive and restart
 * coding, and for regions at iMCU boundaries, image edges and inside blocks. */
namespace {
int checks = 0, failures = 0;
void check(bool value, const std::string& name) {
    ++checks;
    if (!value) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}

struct Variant {
    const char* name;
    int components, h, v;     // Luma sampling factors (chroma is 1x1).
    bool progressive;
    unsigned restart_rows;
    unsigned width, height;
};

/** Smooth gradients plus noise: exercises upsampling across block edges. */
void write_jpeg(const std::filesystem::path& path, const Variant& variant) {
    jpeg_compress_struct codec{};
    jpeg_error_mgr errors{};
    codec.err = jpeg_std_error(&errors);
    jpeg_create_compress(&codec);
    FILE* file = std::fopen(path.c_str(), "wb");
    jpeg_stdio_dest(&codec, file);
    codec.image_width = variant.width;
    codec.image_height = variant.height;
    codec.input_components = variant.components;
    codec.in_color_space = variant.components == 1 ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&codec);
    jpeg_set_quality(&codec, 85, TRUE);
    if (variant.components == 3) {
        codec.comp_info[0].h_samp_factor = variant.h;
        codec.comp_info[0].v_samp_factor = variant.v;
        for (int c = 1; c < 3; ++c) codec.comp_info[c].h_samp_factor = codec.comp_info[c].v_samp_factor = 1;
    }
    if (variant.progressive) jpeg_simple_progression(&codec);
    codec.restart_in_rows = static_cast<int>(variant.restart_rows);
    jpeg_start_compress(&codec, TRUE);
    std::mt19937 noise(variant.width * 31 + variant.height);
    std::vector<JSAMPLE> row(std::size_t(variant.width) * variant.components);
    while (codec.next_scanline < codec.image_height) {
        const unsigned y = codec.next_scanline;
        for (unsigned x = 0; x < variant.width; ++x)
            for (int c = 0; c < variant.components; ++c)
                row[std::size_t(x) * variant.components + c] =
                    static_cast<JSAMPLE>((x * (3 + c) + y * (5 - c) + noise() % 40) % 256);
        JSAMPROW pointer = row.data();
        jpeg_write_scanlines(&codec, &pointer, 1);
    }
    jpeg_finish_compress(&codec);
    jpeg_destroy_compress(&codec);
    std::fclose(file);
}
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    const Variant variants[] = {
        {"gray", 1, 1, 1, false, 0, 237, 151},
        {"444", 3, 1, 1, false, 0, 203, 117},
        {"422", 3, 2, 1, false, 0, 211, 133},
        {"420", 3, 2, 2, false, 0, 333, 251},
        {"440", 3, 1, 2, false, 0, 190, 170},
        {"411", 3, 4, 1, false, 0, 275, 90},
        {"411-progressive", 3, 4, 1, true, 0, 199, 61},
        {"420-progressive", 3, 2, 2, true, 0, 301, 199},
        {"gray-progressive", 1, 1, 1, true, 0, 129, 97},
        {"420-restart", 3, 2, 2, false, 1, 257, 161},
        {"444-restart", 3, 1, 1, false, 2, 145, 113},
        {"420-tiny", 3, 2, 2, false, 0, 17, 9},
    };
    std::mt19937 random(20261123);
    for (const auto& variant : variants) {
        const auto path = directory / (std::string(variant.name) + ".jpg");
        write_jpeg(path, variant);
        const sipi::LocalFileSource source(path);
        const auto metadata = sipi::read_jpeg_metadata(source);
        const auto full = sipi::read_jpeg_image(source, metadata);
        const auto channels = full.channel_count();
        std::vector<sipi::PixelRegion> regions{
            {0, 0, variant.width, variant.height},                  // Whole image.
            {0, 0, 1, 1}, {variant.width - 1, variant.height - 1, 1, 1},  // Corners.
            {16, 16, 16, 16}, {15, 7, 18, 10}, {8, 0, 8, variant.height}, // iMCU edges.
        };
        for (int i = 0; i < 300; ++i) {
            const unsigned x = random() % variant.width, y = random() % variant.height;
            regions.push_back({x, y, 1 + static_cast<unsigned>(random() % (variant.width - x)),
                                     1 + static_cast<unsigned>(random() % (variant.height - y))});
        }
        bool identical = true, covering = true;
        for (const auto& region : regions) {
            if (region.x >= variant.width || region.y >= variant.height ||
                region.width > variant.width - region.x || region.height > variant.height - region.y) continue;
            const auto part = sipi::read_jpeg_region(source, metadata, region);
            covering &= part.x == region.x && part.y == region.y && part.image.height == region.height &&
                        part.image.width == region.width &&
                        part.image.icc_profile == full.icc_profile && part.image.layout == full.layout;
            for (unsigned row = 0; row < part.image.height && identical; ++row) {
                const auto* expected = full.pixels.data() +
                    (std::size_t(part.y + row) * full.width + part.x) * channels;
                const auto* actual = part.image.pixels.data() + std::size_t(row) * part.image.width * channels;
                identical = std::equal(actual, actual + std::size_t(part.image.width) * channels, expected);
            }
        }
        check(covering, std::string(variant.name) + ": result is exactly the requested region");
        check(identical, std::string(variant.name) + ": region pixels equal the full decode");
    }
    // A partial-height crop must validate the tail just like a full decode.
    for (const auto* name : {"444", "420", "420-restart", "420-progressive"}) {
        const auto original = directory / (std::string(name) + ".jpg");
        std::ifstream input(original, std::ios::binary);
        std::string bytes{std::istreambuf_iterator<char>(input), {}};
        for (std::size_t missing : {2u, 100u}) {
            const auto broken = directory / (std::string(name) + "-truncated.jpg");
            { std::ofstream output(broken, std::ios::binary); output.write(bytes.data(), bytes.size() - missing); }
            const sipi::LocalFileSource truncated(broken);
            const auto header = sipi::read_jpeg_metadata(truncated);
            bool rejected = false;
            try { (void)sipi::read_jpeg_region(truncated, header, {0, 0, 1, 1}); }
            catch (const sipi::JpegReadError&) { rejected = true; }
            check(rejected, std::string(name) + ": truncated tail rejected for tiny crop");
        }
    }
    // A nearly full-width crop fits one raster, not two; row-wise copying
    // preserves samples with one accounted row for margins and tail validation.
    {
        const sipi::LocalFileSource source(directory / "444.jpg");
        const auto metadata = sipi::read_jpeg_metadata(source);
        const auto full = sipi::read_jpeg_image(source, metadata);
        const sipi::PixelRegion region{1, 0, metadata.width - 2, metadata.height - 1};
        const auto budget = std::size_t(metadata.width) * metadata.height * 3;
        const auto crop = sipi::read_jpeg_region(source, metadata, region, budget);
        bool identical = true;
        for (unsigned y = 0; y < region.height; ++y)
            identical &= std::equal(crop.image.pixels.begin() + y * region.width * 3,
                crop.image.pixels.begin() + (y + 1) * region.width * 3,
                full.pixels.begin() + (y * metadata.width + 1) * 3);
        check(identical, "tight-budget row-wise crop preserves pixels");
        auto narrow = sipi::read_jpeg_region(source, metadata, {0, 0, 1, 116}).image;
        check(narrow.pixels.capacity() == narrow.pixels.size(), "narrow crop retains no margin allocation");
        const auto resized = sipi::downsample_area(std::move(narrow), 1, 58, 600);
        check(resized.width == 1 && resized.height == 58, "narrow crop fits subsequent resize budget");
    }
    // Invalid regions are rejected before decoding.
    const sipi::LocalFileSource source(directory / "444.jpg");
    const auto metadata = sipi::read_jpeg_metadata(source);
    for (const sipi::PixelRegion invalid : {sipi::PixelRegion{0, 0, 0, 1}, {0, 0, 1, 0},
                                            {203, 0, 1, 1}, {0, 117, 1, 1}, {200, 0, 4, 1}}) {
        bool rejected = false;
        try { (void)sipi::read_jpeg_region(source, metadata, invalid); }
        catch (const sipi::JpegReadError&) { rejected = true; }
        check(rejected, "invalid region rejected");
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
