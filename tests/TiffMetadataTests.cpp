#include "imgformats/TiffMetadata.h"
#include "iiifparser/IIIFGeometry.h"
#include "CommandLine.h"

#include <tiffio.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <string>

namespace {
int checks = 0, failures = 0;
void check(bool ok, const std::string& name) {
    ++checks;
    if (!ok) { ++failures; std::cerr << "FAIL: " << name << '\n'; }
}
using File = std::unique_ptr<TIFF, decltype(&TIFFClose)>;
File create(const std::filesystem::path& path, bool big = false) {
    File file(TIFFOpen(path.string().c_str(), big ? "w8" : "w"), TIFFClose);
    if (!file) { throw std::runtime_error("Cannot write test TIFF"); }
    return file;
}

/** Write small real directories. Compressed payload is deliberately invalid:
 * successful metadata reads must not invoke a pixel decoder. */
void directory(TIFF* file, std::uint32_t width, std::uint32_t height,
               bool tiled = false, bool reduced = false, std::uint16_t orientation = 1,
               bool rgba = false, bool with_profile = false, bool compressed = false) {
    TIFFSetField(file, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(file, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(file, TIFFTAG_SAMPLESPERPIXEL, rgba ? 4 : 3);
    TIFFSetField(file, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(file, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(file, TIFFTAG_ORIENTATION, orientation);
    TIFFSetField(file, TIFFTAG_COMPRESSION, compressed ? COMPRESSION_LZW : COMPRESSION_NONE);
    if (reduced) { TIFFSetField(file, TIFFTAG_SUBFILETYPE, FILETYPE_REDUCEDIMAGE); }
    if (rgba) {
        std::uint16_t alpha = EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(file, TIFFTAG_EXTRASAMPLES, 1, &alpha);
    }
    if (with_profile) {
        std::array<std::uint8_t, 6> profile{1, 0, 3, 0, 5, 6};
        TIFFSetField(file, TIFFTAG_ICCPROFILE, static_cast<std::uint32_t>(profile.size()), profile.data());
    }
    // Only a metadata fixture, not a renderable picture. Even uncompressed data
    // is intentionally shorter than the declared pixel raster.
    std::array<std::uint8_t, 1> raw{0};
    if (tiled) {
        TIFFSetField(file, TIFFTAG_TILEWIDTH, 16);
        TIFFSetField(file, TIFFTAG_TILELENGTH, 16);
        if (TIFFWriteRawTile(file, 0, raw.data(), raw.size()) < 0) { throw std::runtime_error("Write tile failed"); }
    } else {
        TIFFSetField(file, TIFFTAG_ROWSPERSTRIP, 4);
        if (TIFFWriteRawStrip(file, 0, raw.data(), raw.size()) < 0) { throw std::runtime_error("Write strip failed"); }
    }
    if (!TIFFWriteDirectory(file)) { throw std::runtime_error("Write directory failed"); }
}

template<class Function>
void rejects(Function action, const std::string& name) {
    try { action(); check(false, name); }
    catch (const sipi::TiffReadError& error) { check(*error.what() != '\0', name); }
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) { return 2; }
    const std::filesystem::path root{argv[1]};
    std::filesystem::create_directories(root);
    try {
        const auto strips = root / "strips.tif";
        { auto file = create(strips); directory(file.get(), 32, 24, false, false, 1, true, true); }
        const auto info = sipi::read_tiff_metadata(strips);
        check(info.image.width == 32 && info.image.height == 24, "dimensions");
        check(info.image.display_width() == 32 && info.image.display_height() == 24, "normal display dimensions");
        check(info.image.samples_per_pixel == 4 && info.image.bits_per_sample == 8 &&
              info.image.sample_format == SAMPLEFORMAT_UINT && info.image.photometric == PHOTOMETRIC_RGB,
              "pixel interpretation");
        check(info.image.extra_samples == std::vector<std::uint16_t>{EXTRASAMPLE_UNASSALPHA}, "alpha semantics");
        check(info.icc_profile == std::vector<std::uint8_t>{1, 0, 3, 0, 5, 6}, "ICC owned including NULs");
        check(info.image.offset > 0 && info.levels.empty(), "directory offset and no pyramid");
        check(std::get<sipi::TiffStrips>(info.image.layout).rows_per_strip == 4, "strip layout");
        for (std::uint16_t orientation = 1; orientation <= 8; ++orientation) {
            const auto path = root / ("orientation-" + std::to_string(orientation) + ".tif");
            { auto file = create(path); directory(file.get(), 32, 24, true, false, orientation); }
            const auto rotated = sipi::read_tiff_metadata(path);
            check(rotated.image.display_width() == (orientation >= 5 ? 24 : 32) &&
                  rotated.image.display_height() == (orientation >= 5 ? 32 : 24), "orientation dimensions");
            const auto tiles = std::get<sipi::TiffTiles>(rotated.image.layout);
            check(tiles.width == 16 && tiles.height == 16, "tile layout");
        }
        const auto pyramid = root / "pyramid.tif";
        {
            auto file = create(pyramid);
            directory(file.get(), 64, 48, true);
            directory(file.get(), 16, 12, true, true);
            directory(file.get(), 32, 24, true, true);
            directory(file.get(), 10, 10, true, true); // Wrong aspect ratio.
            directory(file.get(), 8, 6, true, true, 6); // Wrong orientation.
            directory(file.get(), 8, 6, true, true, 1, false, true); // Different ICC profile.
            directory(file.get(), 64, 48, true); // Next full page terminates selection.
            directory(file.get(), 8, 6, true, true);
        }
        const auto levels = sipi::read_tiff_metadata(pyramid);
        check(levels.levels.size() == 2 && levels.levels[0].width == 32 && levels.levels[1].width == 16,
              "reduced levels sorted; incompatible and next-page levels excluded");
        const auto multipage = root / "pages.tif";
        { auto file = create(multipage); directory(file.get(), 64, 48); directory(file.get(), 32, 24); }
        check(sipi::read_tiff_metadata(multipage).levels.empty(), "smaller page is not a pyramid");
        const auto subifds = root / "subifds.tif";
        {
            auto file = create(subifds, true);
            std::array<std::uint64_t, 2> offsets{};
            TIFFSetField(file.get(), TIFFTAG_SUBIFD, static_cast<std::uint16_t>(offsets.size()), offsets.data());
            directory(file.get(), 64, 48, true);
            directory(file.get(), 32, 24, true, true);
            directory(file.get(), 16, 12, true, true);
        }
        const auto sublevels = sipi::read_tiff_metadata(subifds);
        check(sublevels.levels.size() == 2 && sublevels.levels[1].width == 16, "BigTIFF SubIFD pyramid");
        const auto compressed = root / "broken-pixels.tif";
        { auto file = create(compressed); directory(file.get(), 32, 24, false, false, 1, false, false, true); }
        check(sipi::read_tiff_metadata(compressed).image.compression == COMPRESSION_LZW,
              "compressed pixel data is not decoded");
        const auto broken = root / "broken-header.tif";
        { std::ofstream file(broken, std::ios::binary); file << "II*"; }
        rejects([&] { (void)sipi::read_tiff_metadata(broken); }, "truncated header");
        rejects([&] { (void)sipi::read_tiff_metadata(root / "absent.tif"); }, "missing file");
        rejects([&] { (void)sipi::read_tiff_metadata(std::filesystem::path(std::string("bad\0path", 8))); }, "NUL in path");

        // Composition test: configuration -> real TIFF metadata -> parsed request
        // -> geometry. The HTTP resource resolver is intentionally not involved.
        const std::array<std::string_view, 2> args{"--size-upscaling", "true"};
        const auto config = sipi::load_startup_options(args);
        const auto& server = std::get<sipi::StartupOptions>(config).config;
        const auto parsed = sipi::iiif::parse_path("/image.tif/16,12,32,24/^32,/0/default.jpg");
        const auto& image = std::get<sipi::iiif::ImageRequest>(parsed);
        const auto geometry = sipi::iiif::resolve_geometry(image.region, image.size,
            info.image.display_width(), info.image.display_height(), {.allow_upscaling = server.size_upscaling});
        const auto& resolved = std::get<sipi::iiif::ImageGeometry>(geometry);
        check(resolved.region.width == 16 && resolved.region.height == 12 &&
              resolved.output_width == 32 && resolved.output_height == 24, "TIFF and configured geometry compose");
        const auto denied = sipi::iiif::resolve_geometry(image.region, image.size, 32, 24);
        check(std::get<sipi::iiif::GeometryError>(denied).code == sipi::iiif::GeometryErrorCode::upscaling_disabled,
              "same request denied with default policy");
        std::vector<std::future<bool>> readers;
        for (int i = 0; i < 8; ++i) {
            readers.push_back(std::async(std::launch::async, [&, i] {
                try {
                    const auto metadata = sipi::read_tiff_metadata(i % 2 == 0 ? subifds : broken);
                    return i % 2 == 0 && metadata.levels.size() == 2;
                } catch (const sipi::TiffReadError&) { return i % 2 != 0; }
            }));
        }
        for (auto& reader : readers) { check(reader.get(), "independent concurrent diagnostics"); }
    } catch (const std::exception& error) {
        check(false, std::string("Unexpected exception: ") + error.what());
    }
    // This test creates only the named fixtures, never removes the parent folder.
    for (const auto* name : {"strips.tif", "pyramid.tif", "pages.tif", "subifds.tif", "broken-pixels.tif", "broken-header.tif"}) {
        std::filesystem::remove(root / name);
    }
    for (int i = 1; i <= 8; ++i) { std::filesystem::remove(root / ("orientation-" + std::to_string(i) + ".tif")); }
    std::cout << checks << " TIFF metadata checks; " << failures << " failures\n";
    return failures ? 1 : 0;
}
