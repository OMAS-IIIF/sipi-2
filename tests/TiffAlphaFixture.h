#pragma once
#include "IccFixture.h"
#include <tiffio.h>
#include <array>
#include <filesystem>
#include <memory>
#include <stdexcept>

namespace fixture {
/** Deliberate hidden blue, opaque red, exact thirds, near-zero and partial alpha. */
inline std::array<std::uint8_t,4> alpha_pixel(unsigned x, unsigned y) {
    switch ((x+y)%5) {
    case 0: return {0,0,255,0};
    case 1: return {255,0,0,255};
    case 2: return {90,180,30,85};
    case 3: return {255,0,255,1};
    default: return {64,128,192,128};
    }
}

/** Create RGB+one extra channel, with explicit/absent alpha declaration. Can
 * corrupt the final compressed tile or a single associated pixel for isolation
 * tests. Reduced dimensions are odd to exercise fractional read windows. */
inline void alpha_directory(TIFF* file, unsigned width, unsigned height, int mode,
                            bool reduced, bool broken, bool invalid_sample) {
    TIFFSetField(file, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(file, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(file, TIFFTAG_SAMPLESPERPIXEL, 4);
    TIFFSetField(file, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(file, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(file, TIFFTAG_COMPRESSION, COMPRESSION_LZW);
    TIFFSetField(file, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(file, TIFFTAG_SUBFILETYPE, reduced ? FILETYPE_REDUCEDIMAGE : 0);
    TIFFSetField(file, TIFFTAG_TILEWIDTH, 16);
    TIFFSetField(file, TIFFTAG_TILELENGTH, 16);
    if (mode >= 0) {
        auto alpha = static_cast<std::uint16_t>(mode);
        TIFFSetField(file, TIFFTAG_EXTRASAMPLES, 1, &alpha);
    }
    if (!reduced) {
        const auto profile = icc_header();
        TIFFSetField(file, TIFFTAG_ICCPROFILE, static_cast<std::uint32_t>(profile.size()), profile.data());
    }
    std::array<std::uint8_t,16*16*4> tile{};
    for (unsigned ty = 0; ty < height; ty += 16)
        for (unsigned tx = 0; tx < width; tx += 16) {
            for (unsigned y = 0; y < 16; ++y)
                for (unsigned x = 0; x < 16; ++x) {
                    auto pixel = alpha_pixel(tx+x, ty+y);
                    if (mode == EXTRASAMPLE_ASSOCALPHA)
                        for (unsigned c = 0; c < 3; ++c)
                            pixel[c] = static_cast<std::uint8_t>((unsigned{pixel[c]}*pixel[3]+127)/255);
                    if (invalid_sample && tx+x == 0 && ty+y == 0) pixel = {255,0,0,0};
                    for (unsigned c = 0; c < 4; ++c) tile[(y*16+x)*4+c] = pixel[c];
                }
            const auto index = TIFFComputeTile(file, tx, ty, 0, 0);
            const auto n = broken && tx+16 >= width && ty+16 >= height
                ? TIFFWriteRawTile(file, index, tile.data(), 1)
                : TIFFWriteEncodedTile(file, index, tile.data(), tile.size());
            if (n < 0) throw std::runtime_error("Cannot write alpha tile");
        }
    if (!TIFFWriteDirectory(file)) throw std::runtime_error("Cannot write alpha IFD");
}
inline void alpha_tiff(const std::filesystem::path& path, int mode = EXTRASAMPLE_UNASSALPHA,
                       bool pyramid = false, bool broken_base = false, bool invalid_sample = false) {
    std::unique_ptr<TIFF,decltype(&TIFFClose)> file(TIFFOpen(path.c_str(), pyramid ? "w8" : "w"), TIFFClose);
    if (!file) throw std::runtime_error("Cannot create alpha fixture");
    std::uint64_t offset = 0;
    if (pyramid) TIFFSetField(file.get(), TIFFTAG_SUBIFD, 1, &offset);
    alpha_directory(file.get(), 35,19,mode,false,broken_base,invalid_sample);
    if (pyramid) alpha_directory(file.get(),18,10,mode,true,false,false);
}
} // namespace fixture
