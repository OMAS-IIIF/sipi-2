#pragma once
#include "IccFixture.h"
#include <tiffio.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <vector>

namespace fixture {
/** Reproducible samples independent of the decoder and normalized to black=0. */
inline std::uint8_t gray_pixel(unsigned x, unsigned y) { return (x*7+y*11)%256; }
struct StripGraySpec {
    bool gray = true;
    bool white_zero = false;
    bool tiled = false;
    bool alpha = false;
    bool associated = false;
    bool broken_last = false;
    bool profile = true;
    std::uint32_t rows = 5;
    std::uint16_t compression = COMPRESSION_LZW;
};
inline std::vector<std::uint8_t> gray_profile() {
    auto profile = icc_header();
    std::copy_n("GRAY", 4, profile.begin()+16);
    return profile;
}
/** Write complete strips/tiles, optionally a damaged final compressed block. */
inline void strip_gray_directory(TIFF* file, unsigned width, unsigned height,
                                 const StripGraySpec& spec, bool reduced = false) {
    const unsigned channels = spec.gray ? 1 : (spec.alpha ? 4 : 3);
    TIFFSetField(file, TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(file, TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(file, TIFFTAG_SAMPLESPERPIXEL, channels);
    TIFFSetField(file, TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(file, TIFFTAG_PHOTOMETRIC, spec.gray ?
        (spec.white_zero ? PHOTOMETRIC_MINISWHITE : PHOTOMETRIC_MINISBLACK) : PHOTOMETRIC_RGB);
    TIFFSetField(file, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(file, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
    TIFFSetField(file, TIFFTAG_COMPRESSION, spec.compression);
    TIFFSetField(file, TIFFTAG_SUBFILETYPE, reduced ? FILETYPE_REDUCEDIMAGE : 0);
    if (spec.compression == COMPRESSION_LZW || spec.compression == COMPRESSION_ADOBE_DEFLATE)
        TIFFSetField(file, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
    if (spec.alpha && !spec.gray) {
        std::uint16_t alpha = spec.associated ? EXTRASAMPLE_ASSOCALPHA : EXTRASAMPLE_UNASSALPHA;
        TIFFSetField(file, TIFFTAG_EXTRASAMPLES, 1, &alpha);
    }
    if (spec.profile && !reduced) {
        const auto profile = spec.gray ? gray_profile() : icc_header();
        TIFFSetField(file, TIFFTAG_ICCPROFILE, static_cast<unsigned>(profile.size()), profile.data());
    }
    if (spec.tiled) {
        TIFFSetField(file, TIFFTAG_TILEWIDTH, 16);
        TIFFSetField(file, TIFFTAG_TILELENGTH, 16);
    } else TIFFSetField(file, TIFFTAG_ROWSPERSTRIP, spec.rows);
    const auto bw = spec.tiled ? 16u : width;
    const auto bh = spec.tiled ? 16u : std::min(spec.rows,height);
    for (unsigned y=0; y<height; y+=bh) for (unsigned x=0; x<width; x+=bw) {
        const auto rows = spec.tiled ? bh : std::min(bh,height-y);
        std::vector<std::uint8_t> block(bw*rows*channels);
        for (unsigned j=0; j<rows; ++j) for (unsigned i=0; i<bw; ++i) {
            const auto value = gray_pixel(x+i,y+j);
            const auto at = (j*bw+i)*channels;
            if (spec.gray) block[at] = spec.white_zero ? 255-value : value;
            else {
                const unsigned alpha = spec.alpha ? ((x+i+y+j)%3)*127 : 255;
                for (unsigned c=0;c<3;++c) {
                    const unsigned color = (value+c*31)%256;
                    block[at+c] = spec.associated ? (color*alpha+127)/255 : color;
                }
                if (spec.alpha) block[at+3] = alpha;
            }
        }
        const bool broken = spec.broken_last && y+bh>=height && x+bw>=width;
        const auto index = spec.tiled ? TIFFComputeTile(file,x,y,0,0) : TIFFComputeStrip(file,y,0);
        const auto n = spec.tiled ?
            (broken ? TIFFWriteRawTile(file,index,block.data(),1) : TIFFWriteEncodedTile(file,index,block.data(),block.size())) :
            (broken ? TIFFWriteRawStrip(file,index,block.data(),1) : TIFFWriteEncodedStrip(file,index,block.data(),block.size()));
        if (n<0) throw std::runtime_error("Cannot write strip/gray fixture");
    }
    if (!TIFFWriteDirectory(file)) throw std::runtime_error("Cannot write strip/gray IFD");
}
inline void strip_gray_tiff(const std::filesystem::path& path, const StripGraySpec& spec = {},
                            bool pyramid = false, const char* mode = "w") {
    std::unique_ptr<TIFF,decltype(&TIFFClose)> file(TIFFOpen(path.c_str(),mode),TIFFClose);
    if (!file) throw std::runtime_error("Cannot create strip/gray fixture");
    std::uint64_t offset = 0;
    if (pyramid) TIFFSetField(file.get(), TIFFTAG_SUBIFD, 1, &offset);
    strip_gray_directory(file.get(),37,23,spec);
    if (pyramid) {
        auto level = spec; level.broken_last = false;
        strip_gray_directory(file.get(),19,12,level,true);
    }
}
} // namespace fixture
