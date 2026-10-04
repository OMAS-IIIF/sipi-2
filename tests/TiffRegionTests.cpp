#include "imgformats/TiffReader.h"
#include "storage/LocalFileSource.h"
#include "iiifparser/IIIFGeometry.h"

#include <tiffio.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <string>

namespace {
int checks = 0, failures = 0;
void check(bool condition, const std::string& name) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}

/** Deliberately different channel patterns catch offset and channel-order bugs. */
std::array<std::uint8_t, 3> pixel(std::uint32_t x, std::uint32_t y) {
    return {static_cast<std::uint8_t>((x * 7 + y * 3) % 256),
            static_cast<std::uint8_t>((x + y * 11 + 19) % 256),
            static_cast<std::uint8_t>((x * 13 + y * 17 + 41) % 256)};
}
struct Fixture {
    bool big = false;
    bool tiled = true;
    bool corrupt_last_tile = false;
    std::uint16_t compression = COMPRESSION_NONE;
    std::uint16_t bits = 8;
    std::uint16_t samples = 3;
    std::uint16_t planar = PLANARCONFIG_CONTIG;
    std::uint16_t orientation = ORIENTATION_TOPLEFT;
    std::uint16_t photometric = PHOTOMETRIC_RGB;
    std::uint16_t sample_format = SAMPLEFORMAT_UINT;
};
constexpr std::uint32_t width = 53, height = 41, tile_width = 16, tile_height = 32;
const std::vector<std::uint8_t> profile{1, 0, 3, 4, 0, 6};

/** Write independent known pixel data, including partial edge tiles. Padding uses
 * a poison value so copying past the image edge is visible in comparisons. */
void write_fixture(const std::filesystem::path& path, const Fixture& spec) {
    std::unique_ptr<TIFF, decltype(&TIFFClose)> file(TIFFOpen(path.string().c_str(), spec.big ? "w8" : "w"), TIFFClose);
    if (!file) { throw std::runtime_error("Cannot create TIFF fixture"); }
    TIFFSetField(file.get(), TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(file.get(), TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(file.get(), TIFFTAG_SAMPLESPERPIXEL, spec.samples);
    TIFFSetField(file.get(), TIFFTAG_BITSPERSAMPLE, spec.bits);
    TIFFSetField(file.get(), TIFFTAG_SAMPLEFORMAT, spec.sample_format);
    TIFFSetField(file.get(), TIFFTAG_PLANARCONFIG, spec.planar);
    TIFFSetField(file.get(), TIFFTAG_PHOTOMETRIC, spec.photometric);
    TIFFSetField(file.get(), TIFFTAG_ORIENTATION, spec.orientation);
    TIFFSetField(file.get(), TIFFTAG_COMPRESSION, spec.compression);
    TIFFSetField(file.get(), TIFFTAG_ICCPROFILE, static_cast<std::uint32_t>(profile.size()), profile.data());
    if (spec.samples == 4) {
        std::uint16_t alpha = EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(file.get(), TIFFTAG_EXTRASAMPLES, 1, &alpha);
    }
    if (!spec.tiled) {
        TIFFSetField(file.get(), TIFFTAG_ROWSPERSTRIP, 4);
        std::vector<std::uint8_t> row(static_cast<std::size_t>(TIFFScanlineSize(file.get())), 0);
        for (std::uint32_t y = 0; y < height; ++y) {
            if (TIFFWriteScanline(file.get(), row.data(), y, 0) < 0) { throw std::runtime_error("Cannot write strip fixture"); }
        }
        return;
    }
    TIFFSetField(file.get(), TIFFTAG_TILEWIDTH, tile_width);
    TIFFSetField(file.get(), TIFFTAG_TILELENGTH, tile_height);
    std::vector<std::uint8_t> tile(static_cast<std::size_t>(TIFFTileSize(file.get())), 0xee);
    const std::uint16_t planes = spec.planar == PLANARCONFIG_SEPARATE ? spec.samples : 1;
    for (std::uint16_t plane = 0; plane < planes; ++plane) {
        for (std::uint32_t ty = 0; ty < height; ty += tile_height) {
            for (std::uint32_t tx = 0; tx < width; tx += tile_width) {
                std::fill(tile.begin(), tile.end(), 0xee);
                if (spec.bits == 8 && spec.samples == 3 && spec.planar == PLANARCONFIG_CONTIG) {
                    for (std::uint32_t y = 0; y < tile_height && ty + y < height; ++y) {
                        for (std::uint32_t x = 0; x < tile_width && tx + x < width; ++x) {
                            const auto rgb = pixel(tx + x, ty + y);
                            for (std::size_t c = 0; c < 3; ++c) { tile[(y * tile_width + x) * 3 + c] = rgb[c]; }
                        }
                    }
                }
                const auto index = TIFFComputeTile(file.get(), tx, ty, 0, plane);
                tmsize_t written;
                if (spec.corrupt_last_tile && tx == 48 && ty == 32) {
                    std::uint8_t garbage = 0;
                    written = TIFFWriteRawTile(file.get(), index, &garbage, 1);
                } else {
                    written = TIFFWriteEncodedTile(file.get(), index, tile.data(), static_cast<tmsize_t>(tile.size()));
                }
                if (written < 0) { throw std::runtime_error("Cannot write tile fixture"); }
            }
        }
    }
}

void verify(const std::filesystem::path& path, const sipi::PixelRegion& region) {
    const auto result = sipi::read_tiff_region(sipi::LocalFileSource{path}, region);
    bool identical = result.width == region.width && result.height == region.height &&
        result.pixels.size() == static_cast<std::size_t>(region.width) * region.height * 3 && result.icc_profile == profile;
    for (std::uint32_t y = 0; identical && y < region.height; ++y) {
        for (std::uint32_t x = 0; identical && x < region.width; ++x) {
            const auto expected = pixel(region.x + x, region.y + y);
            for (std::size_t c = 0; c < 3; ++c) {
                identical = identical && result.pixels[(static_cast<std::size_t>(y) * region.width + x) * 3 + c] == expected[c];
            }
        }
    }
    check(identical, "pixel equality: " + path.filename().string() + " at " + std::to_string(region.x) + "," + std::to_string(region.y));
}

template<class Function>
void rejects(Function action, const std::string& expected) {
    try { action(); check(false, "expected failure: " + expected); }
    catch (const sipi::TiffReadError& error) {
        check(std::string(error.what()).find(expected) != std::string::npos, error.what());
    }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) { return 2; }
    const std::filesystem::path root{argv[1]};
    std::filesystem::create_directories(root);
    std::vector<std::filesystem::path> files;
    const auto make = [&](const std::string& name, const Fixture& spec) {
        const auto path = root / name;
        files.push_back(path);
        write_fixture(path, spec);
        return path;
    };
    try {
        for (const auto compression : {COMPRESSION_NONE, COMPRESSION_LZW, COMPRESSION_ADOBE_DEFLATE, COMPRESSION_PACKBITS}) {
            const auto path = make("rgb-" + std::to_string(compression) + ".tif", {.compression = static_cast<std::uint16_t>(compression)});
            for (const sipi::PixelRegion region : {sipi::PixelRegion{0, 0, width, height}, {0, 0, 1, 1},
                 {52, 40, 1, 1}, {16, 0, 16, 32}, {15, 31, 2, 2}, {33, 30, 20, 11},
                 {48, 32, 5, 9}, {31, 0, 1, 41}, {0, 31, 53, 1}}) {
                verify(path, region);
            }
            // Deterministic crops cover starts beyond the first tile and different spans.
            for (std::uint32_t n = 0; n < 100; ++n) {
                const auto x = (n * 17) % width, y = (n * 23) % height;
                verify(path, {x, y, 1 + (n * 7) % (width - x), 1 + (n * 11) % (height - y)});
            }
        }
        const auto big = make("big.tif", {.big = true, .compression = COMPRESSION_LZW});
        verify(big, {33, 30, 20, 11});
        const auto broken = make("corrupt-last.tif", {.corrupt_last_tile = true, .compression = COMPRESSION_LZW});
        verify(broken, {0, 0, 16, 32}); // Proves the unrelated damaged tile is never decoded.
        rejects([&] { (void)sipi::read_tiff_region(sipi::LocalFileSource{broken}, {48, 32, 5, 9}); }, "Cannot decode complete TIFF tile");
        const auto short_tile = make("short-last.tif", {.corrupt_last_tile = true});
        rejects([&] { (void)sipi::read_tiff_region(sipi::LocalFileSource{short_tile}, {48, 32, 5, 9}); }, "Cannot decode complete TIFF tile");
        const std::vector<Fixture> unsupported{
            {.tiled = false}, {.bits = 16}, {.samples = 4}, {.planar = PLANARCONFIG_SEPARATE},
            {.orientation = ORIENTATION_RIGHTTOP}, {.samples = 1, .photometric = PHOTOMETRIC_MINISBLACK},
            {.sample_format = SAMPLEFORMAT_INT}};
        for (std::size_t i = 0; i < unsupported.size(); ++i) {
            const auto path = make("unsupported-" + std::to_string(i) + ".tif", unsupported[i]);
            rejects([&] { (void)sipi::read_tiff_region(sipi::LocalFileSource{path}, {0, 0, 1, 1}); }, "Unsupported TIFF");
        }
        for (const sipi::PixelRegion region : {sipi::PixelRegion{0, 0, 0, 1}, {0, 0, 1, 0},
             {53, 0, 1, 1}, {0, 41, 1, 1}, {52, 40, 2, 1}, {0, 0, std::numeric_limits<std::uint32_t>::max(), 1}}) {
            rejects([&] { (void)sipi::read_tiff_region(sipi::LocalFileSource{big}, region); }, "crop must be nonempty");
        }
        const std::size_t budget = 16 * 32 * 3 + 3;
        const auto one = sipi::read_tiff_region(sipi::LocalFileSource{big}, {0, 0, 1, 1}, budget);
        check(one.pixels.size() == 3, "exact combined allocation budget");
        rejects([&] { (void)sipi::read_tiff_region(sipi::LocalFileSource{big}, {0, 0, 1, 1}, budget - 1); }, "decoded-byte limit");
        rejects([&] { (void)sipi::read_tiff_region(sipi::LocalFileSource{big}, {0, 0, 1, 1}, 0); }, "decoded-byte limit");
        const auto metadata = sipi::read_tiff_metadata(sipi::LocalFileSource{big});
        const auto parsed = sipi::iiif::parse_path("/id/33,30,100,100/max/0/default.jpg");
        const auto& request = std::get<sipi::iiif::ImageRequest>(parsed);
        const auto resolved = sipi::iiif::resolve_geometry(request.region, request.size,
            metadata.image.display_width(), metadata.image.display_height());
        const auto& geometry = std::get<sipi::iiif::ImageGeometry>(resolved);
        verify(big, geometry.region);
        check(geometry.output_width == 20 && geometry.output_height == 11, "metadata -> IIIF geometry -> TIFF crop");
    } catch (const std::exception& error) {
        check(false, std::string("Unexpected exception: ") + error.what());
    }
    for (const auto& path : files) { std::filesystem::remove(path); }
    std::cout << checks << " TIFF region checks; " << failures << " failures\n";
    return failures ? 1 : 0;
}
