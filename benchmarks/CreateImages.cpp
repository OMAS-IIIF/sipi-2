#include <grok.h>
#include <tiffio.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
constexpr unsigned side = 4096;
constexpr unsigned tile_side = 256;
using Pixels = std::vector<std::uint8_t>;

/** Fixed RGB8 synthetic texture: gradients, edges and bounded noise. No external
 * image licensing or random seed dependencies. Not representative of all masters. */
Pixels make_pixels() {
    Pixels pixels(side * side * 3);
    for (unsigned y = 0; y < side; ++y) for (unsigned x = 0; x < side; ++x) {
        std::uint32_t hash = x * 374761393u + y * 668265263u;
        hash = (hash ^ (hash >> 13)) * 1274126177u;
        const unsigned noise = (hash ^ (hash >> 16)) & 31;
        for (unsigned c = 0; c < 3; ++c)
            pixels[(y * side + x) * 3 + c] = static_cast<std::uint8_t>(
                ((x / (11 + c * 7) + y / (17 + c * 3) + ((x / 128) ^ (y / 128)) * 23) % 224) + noise);
    }
    return pixels;
}

/** Top-left RGB8/LZW/256px tiles, main IFD plus reduced IFD chain to 256px.
 * Each level is a rounded 2x2 average of its parent; all levels are real images. */
void write_tiff(const std::filesystem::path& path, Pixels pixels) {
    std::unique_ptr<TIFF, decltype(&TIFFClose)> file(TIFFOpen(path.c_str(), "w"), TIFFClose);
    if (!file) throw std::runtime_error("Cannot create benchmark TIFF");
    std::vector<std::uint8_t> tile(tile_side * tile_side * 3);
    for (unsigned size = side; size >= tile_side; size /= 2) {
        if (!(TIFFSetField(file.get(), TIFFTAG_IMAGEWIDTH, size) &&
              TIFFSetField(file.get(), TIFFTAG_IMAGELENGTH, size) &&
              TIFFSetField(file.get(), TIFFTAG_SAMPLESPERPIXEL, 3) &&
              TIFFSetField(file.get(), TIFFTAG_BITSPERSAMPLE, 8) &&
              TIFFSetField(file.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB) &&
              TIFFSetField(file.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG) &&
              TIFFSetField(file.get(), TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT) &&
              TIFFSetField(file.get(), TIFFTAG_COMPRESSION, COMPRESSION_LZW) &&
              TIFFSetField(file.get(), TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL) &&
              TIFFSetField(file.get(), TIFFTAG_TILEWIDTH, tile_side) &&
              TIFFSetField(file.get(), TIFFTAG_TILELENGTH, tile_side) &&
              TIFFSetField(file.get(), TIFFTAG_SUBFILETYPE, size == side ? 0 : FILETYPE_REDUCEDIMAGE)))
            throw std::runtime_error("Cannot set benchmark TIFF tags");
        for (unsigned y = 0; y < size; y += tile_side) for (unsigned x = 0; x < size; x += tile_side) {
            for (unsigned row = 0; row < tile_side; ++row)
                std::copy_n(pixels.data() + ((y + row) * size + x) * 3, tile_side * 3,
                            tile.data() + row * tile_side * 3);
            if (TIFFWriteEncodedTile(file.get(), TIFFComputeTile(file.get(), x, y, 0, 0),
                                     tile.data(), static_cast<tmsize_t>(tile.size())) < 0)
                throw std::runtime_error("Cannot encode benchmark TIFF tile");
        }
        if (!TIFFWriteDirectory(file.get())) throw std::runtime_error("Cannot finish benchmark TIFF directory");
        if (size == tile_side) break;
        const auto next = size / 2;
        Pixels reduced(next * next * 3);
        for (unsigned y = 0; y < next; ++y) for (unsigned x = 0; x < next; ++x)
            for (unsigned c = 0; c < 3; ++c) {
                unsigned sum = 2;
                for (unsigned dy = 0; dy < 2; ++dy) for (unsigned dx = 0; dx < 2; ++dx)
                    sum += pixels[((y * 2 + dy) * size + x * 2 + dx) * 3 + c];
                reduced[(y * next + x) * 3 + c] = static_cast<std::uint8_t>(sum / 4);
            }
        pixels = std::move(reduced);
    }
}

/** Lossless RGB8 JP2: 1024px tiles, six resolutions, reversible wavelet.
 * Encoding is benchmark setup only, not a SIPI output feature. */
void write_jp2(const std::filesystem::path& path, const Pixels& pixels) {
    std::vector<grk_image_comp> components(3);
    for (auto& c : components) { c.w = c.h = side; c.prec = 8; c.dx = c.dy = 1; }
    const auto destroy = [](grk_image* image) { if (image) grk_object_unref(&image->obj); };
    std::unique_ptr<grk_image, decltype(destroy)> image(
        grk_image_new(3, components.data(), GRK_CLRSPC_SRGB, true), destroy);
    if (!image) throw std::runtime_error("Cannot allocate JP2 image");
    image->x1 = image->y1 = side;
    for (unsigned c = 0; c < 3; ++c)
        for (unsigned y = 0; y < side; ++y) for (unsigned x = 0; x < side; ++x)
            static_cast<std::int32_t*>(image->comps[c].data)[y * image->comps[c].stride + x] =
                pixels[(y * side + x) * 3 + c];
    // Bounded destination large enough for this fixed fixture; validate encoder size.
    std::vector<std::uint8_t> encoded(pixels.size() * 2);
    grk_stream_params stream{};
    stream.buf = encoded.data(); stream.buf_len = encoded.size();
    grk_cparameters params{};
    grk_compress_set_default_params(&params);
    params.cod_format = GRK_FMT_JP2;
    params.numresolution = 6;
    params.irreversible = false;
    params.tile_size_on = true; params.t_width = params.t_height = 1024;
    const std::unique_ptr<grk_object, decltype(&grk_object_unref)> codec(
        grk_compress_init(&stream, &params, image.get()), grk_object_unref);
    if (!codec) throw std::runtime_error("Cannot initialize benchmark JP2 encoder");
    const auto count = grk_compress(codec.get(), nullptr);
    if (!count || count > encoded.size()) throw std::runtime_error("Cannot encode benchmark JP2");
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(encoded.data()), static_cast<std::streamsize>(count));
    if (!file) throw std::runtime_error("Cannot write benchmark JP2");
}
}

int main(int argc, char** argv) {
    if (argc != 2) { std::cerr << "Usage: benchmark-images NEW_DIRECTORY\n"; return 2; }
    grk_initialize(nullptr, 1, nullptr);
    try {
        const std::filesystem::path directory(argv[1]);
        // Refuse replacement: fixtures and timings must have traceable provenance.
        if (!std::filesystem::create_directory(directory)) throw std::runtime_error("Output directory must be new");
        const auto pixels = make_pixels();
        write_tiff(directory / "master.tif", pixels);
        write_jp2(directory / "master.jp2", pixels);
        std::cout << "Generated 4096x4096 RGB8 TIFF pyramid and lossless JP2\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; grk_deinitialize(); return 1;
    }
    grk_deinitialize();
}
