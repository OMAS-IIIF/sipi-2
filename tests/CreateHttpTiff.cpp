#include <tiffio.h>
#include <algorithm>
#include <filesystem>
#include <memory>
#include <vector>
#include <stdexcept>
#include <iostream>

/** Generate a small, real RGB gradient TIFF. These deterministic fixtures need
 * no network/download license and can also be used for manual HTTP requests. */
void write_image(const std::filesystem::path& path, const char* mode,
                 std::uint32_t width, std::uint32_t height, std::uint16_t orientation,
                 bool tiled = false, bool broken = false) {
    std::unique_ptr<TIFF, decltype(&TIFFClose)> file(TIFFOpen(path.c_str(), mode), TIFFClose);
    if (!file) throw std::runtime_error("cannot create TIFF fixture");
    TIFFSetField(file.get(), TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(file.get(), TIFFTAG_IMAGELENGTH, height);
    TIFFSetField(file.get(), TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(file.get(), TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(file.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(file.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(file.get(), TIFFTAG_COMPRESSION, broken ? COMPRESSION_LZW : COMPRESSION_NONE);
    if (!tiled) TIFFSetField(file.get(), TIFFTAG_ROWSPERSTRIP, 16);
    TIFFSetField(file.get(), TIFFTAG_ORIENTATION, orientation);
    if (tiled) {
        constexpr std::uint32_t side = 64;
        TIFFSetField(file.get(), TIFFTAG_TILEWIDTH, side);
        TIFFSetField(file.get(), TIFFTAG_TILELENGTH, side);
        std::vector<unsigned char> tile(side * side * 3);
        for (std::uint32_t ty = 0; ty < height; ty += side) {
            for (std::uint32_t tx = 0; tx < width; tx += side) {
                std::fill(tile.begin(), tile.end(), 0);
                for (std::uint32_t y = 0; y < side && ty+y < height; ++y) {
                    for (std::uint32_t x = 0; x < side && tx+x < width; ++x) {
                        tile[(y*side+x)*3] = static_cast<unsigned char>(255*(tx+x)/(width-1));
                        tile[(y*side+x)*3+1] = static_cast<unsigned char>(255*(ty+y)/(height-1));
                        tile[(y*side+x)*3+2] = 128;
                    }
                }
                const auto index = TIFFComputeTile(file.get(), tx, ty, 0, 0);
                const auto written = broken && tx == 0 && ty == 0
                    ? TIFFWriteRawTile(file.get(), index, tile.data(), 1)
                    : TIFFWriteEncodedTile(file.get(), index, tile.data(), static_cast<tmsize_t>(tile.size()));
                if (written < 0) throw std::runtime_error("cannot write TIFF tile fixture");
            }
        }
        return;
    }
    std::vector<unsigned char> row(width * 3);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            row[3*x] = static_cast<unsigned char>(255*x/(width-1));
            row[3*x+1] = static_cast<unsigned char>(255*y/(height-1));
            row[3*x+2] = 128;
        }
        if (TIFFWriteScanline(file.get(), row.data(), y, 0) < 0)
            throw std::runtime_error("cannot write TIFF fixture");
    }
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        const std::filesystem::path directory(argv[1]);
        std::filesystem::create_directories(directory);
        write_image(directory/"tiled.tif", "w", 320, 200, ORIENTATION_TOPLEFT, true);
        write_image(directory/"broken-tile.tif", "w", 320, 200, ORIENTATION_TOPLEFT, true, true);
        write_image(directory/"sample.tif", "w", 320, 200, ORIENTATION_TOPLEFT);
        write_image(directory/"rotated.tif", "w", 320, 200, ORIENTATION_RIGHTTOP);
        write_image(directory/"large.tif", "w8", 1000, 800, ORIENTATION_TOPLEFT);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
