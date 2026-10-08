#pragma once
#include <tiffio.h>
#include "IccFixture.h"
#include <array>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

namespace fixture {
/** Distinct level/channel values make accidental base decoding observable. */
inline std::array<std::uint8_t, 3> pyramid_pixel(std::uint32_t x, std::uint32_t y, unsigned level) {
    return {static_cast<std::uint8_t>(x * 3 + level * 20),
            static_cast<std::uint8_t>(y * 4 + level * 30), 100};
}

/** Renderable RGB8 level, except an optional deliberately corrupt final tile.
 * Main-image ICC is deliberately opaque test data; reduced IFDs omit it. */
inline void pyramid_directory(TIFF* file, std::uint32_t width, std::uint32_t height,
                              unsigned level, bool broken = false, bool stripped = false,
                              bool profile = false) {
    TIFFSetField(file, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(file, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(file, TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(file, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(file, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(file, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
    TIFFSetField(file, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(file, TIFFTAG_SUBFILETYPE, level ? FILETYPE_REDUCEDIMAGE : 0);
    if (profile) {
        const auto bytes = icc_header();
        TIFFSetField(file, TIFFTAG_ICCPROFILE, static_cast<std::uint32_t>(bytes.size()), bytes.data());
    }
    if (stripped) {
        TIFFSetField(file, TIFFTAG_ROWSPERSTRIP, 16);
        std::vector<std::uint8_t> row(width * 3);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto pixel = pyramid_pixel(x, y, level);
                for (unsigned c = 0; c < 3; ++c) row[x*3+c] = pixel[c];
            }
            if (TIFFWriteScanline(file, row.data(), y) < 0) throw std::runtime_error("Fixture strip failed");
        }
    } else {
        TIFFSetField(file, TIFFTAG_TILEWIDTH, 16);
        TIFFSetField(file, TIFFTAG_TILELENGTH, 16);
        std::array<std::uint8_t, 16*16*3> tile{};
        for (std::uint32_t y = 0; y < height; y += 16)
            for (std::uint32_t x = 0; x < width; x += 16) {
                for (unsigned j = 0; j < 16; ++j)
                    for (unsigned i = 0; i < 16; ++i) {
                        const auto pixel = pyramid_pixel(x+i, y+j, level);
                        for (unsigned c = 0; c < 3; ++c) tile[(j*16+i)*3+c] = pixel[c];
                    }
                const auto index = TIFFComputeTile(file, x, y, 0, 0);
                const auto count = broken && x+16 >= width && y+16 >= height
                    ? TIFFWriteRawTile(file, index, tile.data(), 1)
                    : TIFFWriteEncodedTile(file, index, tile.data(), tile.size());
                if (count < 0) throw std::runtime_error("Fixture tile failed");
            }
    }
    if (!TIFFWriteDirectory(file)) throw std::runtime_error("Fixture directory failed");
}

/** Odd-size classic/main-chain or BigTIFF/SubIFD pyramid; no external assets. */
inline void pyramid(const std::filesystem::path& path, bool subifds = false,
                    bool broken_base = false, bool stripped_smallest = false,
                    bool broken_smallest = false, bool profile = true) {
    std::unique_ptr<TIFF, decltype(&TIFFClose)> file(TIFFOpen(path.c_str(), subifds ? "w8" : "w"), TIFFClose);
    if (!file) throw std::runtime_error("Cannot create pyramid");
    std::array<std::uint64_t, 2> offsets{};
    if (subifds) TIFFSetField(file.get(), TIFFTAG_SUBIFD, 2, offsets.data());
    pyramid_directory(file.get(), 101, 77, 0, broken_base, false, profile);
    pyramid_directory(file.get(), 51, 39, 1);
    pyramid_directory(file.get(), 26, 20, 2, broken_smallest, stripped_smallest);
}
} // namespace fixture
