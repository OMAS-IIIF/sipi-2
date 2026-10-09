#include "imgformats/JpegWriter.h"
#include <cstddef>
#include <cstdio>
#include <jpeglib.h>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

/** Independent libjpeg decompression checks output pixels and saved ICC chunks.
 * This is a test process: libjpeg's default fatal handler exits it on bad JPEGs. */
sipi::RasterImage8 decode(const std::vector<unsigned char>& bytes, bool require_gray = false, int y_sampling = 1) {
    jpeg_decompress_struct codec{};
    jpeg_error_mgr error{};
    codec.err = jpeg_std_error(&error);
    jpeg_create_decompress(&codec);
    struct Cleanup {
        jpeg_decompress_struct* codec;
        ~Cleanup() { jpeg_destroy_decompress(codec); }
    } cleanup{&codec};
    jpeg_mem_src(&codec, bytes.data(), bytes.size());
    jpeg_save_markers(&codec, JPEG_APP0 + 2, 65535);
    require(jpeg_read_header(&codec, TRUE) == JPEG_HEADER_OK, "JPEG header");
    if (require_gray) require(codec.num_components == 1 && codec.jpeg_color_space == JCS_GRAYSCALE, "Native grayscale JPEG");
    for (int i = 0; i < codec.num_components; ++i) {
        const int factor = i == 0 && codec.num_components == 3 ? y_sampling : 1;
        require(codec.comp_info[i].h_samp_factor == factor &&
                codec.comp_info[i].v_samp_factor == factor, "JPEG sampling factors");
    }
    codec.out_color_space = JCS_RGB;
    jpeg_start_decompress(&codec);
    sipi::RasterImage8 image{codec.output_width, codec.output_height, {}, {}};
    image.pixels.resize(static_cast<std::size_t>(image.width) * image.height * 3);
    unsigned sequence = 1;
    unsigned total = 0;
    for (auto* marker = codec.marker_list; marker; marker = marker->next) {
        require(marker->data_length >= 14, "ICC marker length");
        require(std::string(reinterpret_cast<char*>(marker->data), 12) == std::string("ICC_PROFILE\0", 12), "ICC signature");
        require(marker->data[12] == sequence++, "ICC chunk order");
        if (!total) total = marker->data[13];
        require(total == marker->data[13], "ICC chunk count");
        image.icc_profile.insert(image.icc_profile.end(), marker->data + 14, marker->data + marker->data_length);
    }
    require(sequence - 1 == total, "ICC complete");
    while (codec.output_scanline < codec.output_height) {
        auto* row = image.pixels.data() + static_cast<std::size_t>(codec.output_scanline) * image.width * 3;
        jpeg_read_scanlines(&codec, &row, 1);
    }
    jpeg_finish_decompress(&codec);
    return image;
}

/** Compare lossy decoded pixels to the global gradient, including crop origin. */
void gradient(const sipi::RasterImage8& image, unsigned ox, unsigned oy, unsigned width, unsigned height) {
    require(image.width == width && image.height == height, "JPEG dimensions");
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned x = 0; x < width; ++x) {
            const auto i = (static_cast<std::size_t>(y) * width + x) * 3;
            require(std::abs(int(image.pixels[i]) - int(255*(x+ox)/319)) <= 5, "red crop pixel");
            require(std::abs(int(image.pixels[i+1]) - int(255*(y+oy)/199)) <= 5, "green crop pixel");
            require(std::abs(int(image.pixels[i+2]) - 128) <= 5, "blue crop pixel");
        }
    }
}
/** Independent common-grid averaging of the known separable TIFF gradient. */
void resized_gradient(const sipi::RasterImage8& image, unsigned ox, unsigned oy,
                      unsigned sw, unsigned sh, unsigned w, unsigned h) {
    require(image.width == w && image.height == h, "Resized JPEG dimensions");
    for (unsigned y = 0; y < h; ++y)
        for (unsigned x = 0; x < w; ++x) {
            unsigned red = 0, green = 0;
            for (unsigned i = 0; i < sw; ++i) red += 255 * (ox + (x * sw + i) / w) / 319;
            for (unsigned j = 0; j < sh; ++j) green += 255 * (oy + (y * sh + j) / h) / 199;
            const auto index = (static_cast<std::size_t>(y) * w + x) * 3;
            require(std::abs(int(image.pixels[index]) - int((2 * red + sw) / (2 * sw))) <= 5, "Scaled red pixel");
            require(std::abs(int(image.pixels[index + 1]) - int((2 * green + sh) / (2 * sh))) <= 5, "Scaled green pixel");
            require(std::abs(int(image.pixels[index + 2]) - 128) <= 5, "Scaled blue pixel");
        }
}

}

/** Without arguments: encoder contract tests. With a file and crop coordinates:
 * decode an HTTP response and validate the known TIFF fixture's actual pixels. */
int main(int argc, char** argv) {
    try {
        if (argc == 6 && (std::string(argv[1]) == "--rgb" || std::string(argv[1]) == "--gray")) {
            std::ifstream input(argv[2], std::ios::binary), reference(argv[3], std::ios::binary);
            require(bool(input) && bool(reference), "Cannot open JPEG reference files");
            const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input), {}};
            const std::vector<unsigned char> expected{std::istreambuf_iterator<char>(reference), {}};
            const auto image = decode(bytes, std::string(argv[1]) == "--gray");
            require(image.width == std::stoul(argv[4]) && image.height == std::stoul(argv[5]) &&
                    image.pixels.size() == expected.size(), "JPEG reference dimensions");
            if (std::string(argv[1]) == "--gray")
                require(image.icc_profile.size() >= 132 && std::string(image.icc_profile.begin()+16,image.icc_profile.begin()+20)=="GRAY", "Gray ICC in JPEG");
            for (std::size_t i=0; i<expected.size(); ++i)
                require(std::abs(int(image.pixels[i])-int(expected[i])) <= 35, "JPEG composited pixel");
            return 0;
        }
        if (argc == 6 || argc == 8) {
            std::ifstream input(argv[1], std::ios::binary);
            require(bool(input), "cannot open HTTP JPEG");
            const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(input), {}};
            if (argc == 8) {
                resized_gradient(decode(bytes), std::stoul(argv[2]), std::stoul(argv[3]), std::stoul(argv[4]),
                                 std::stoul(argv[5]), std::stoul(argv[6]), std::stoul(argv[7]));
                return 0;
            }
            gradient(decode(bytes), std::stoul(argv[2]), std::stoul(argv[3]), std::stoul(argv[4]), std::stoul(argv[5]));
            return 0;
        }
        require(argc == 1, "unexpected arguments");
        sipi::RasterImage8 image{320, 200, {}, {}};
        for (unsigned y = 0; y < image.height; ++y) {
            for (unsigned x = 0; x < image.width; ++x) {
                image.pixels.push_back(static_cast<unsigned char>(255*x/319));
                image.pixels.push_back(static_cast<unsigned char>(255*y/199));
                image.pixels.push_back(128);
            }
        }
        // Opaque synthetic ICC bytes verify preservation/chunking, not color validity.
        image.icc_profile.resize(140000);
        for (std::size_t i = 0; i < image.icc_profile.size(); ++i) image.icc_profile[i] = static_cast<unsigned char>(i);
        std::vector<unsigned char> bytes;
        unsigned blocks = 0;
        const sipi::JpegSink collect = [&](auto part) {
            require(!part.empty() && part.size() <= 16384, "bounded output block");
            ++blocks;
            bytes.insert(bytes.end(), part.begin(), part.end());
        };
        sipi::write_jpeg(image, collect);
        require(blocks > 2, "multiple output blocks");
        const auto decoded = decode(bytes);
        gradient(decoded, 0, 0, 320, 200);
        require(decoded.icc_profile == image.icc_profile, "ICC preserved");
        const auto default_bytes = bytes;
        bytes.clear();
        sipi::write_jpeg(image, collect, {90, sipi::JpegSampling::yuv444});
        require(bytes == default_bytes, "Explicit defaults preserve bytes");
        // Odd edges exercise chroma padding without changing delivered dimensions.
        auto odd = image;
        odd.width = 319; odd.height = 199;
        odd.pixels.resize(static_cast<std::size_t>(odd.width) * odd.height * 3);
        for (int quality : {1, 75, 100}) {
            bytes.clear();
            sipi::write_jpeg(odd, collect, {quality, sipi::JpegSampling::yuv420});
            const auto output = decode(bytes, false, 2);
            require(output.width == odd.width && output.height == odd.height, "Odd 4:2:0 dimensions");
            require(output.icc_profile == odd.icc_profile, "4:2:0 ICC preserved");
        }
        sipi::RasterImage8 gray{13, 9, std::vector<std::uint8_t>(13*9, 127), {}};
        gray.layout = sipi::PixelLayout::gray;
        bytes.clear();
        sipi::write_jpeg(gray, collect);
        const auto gray_bytes = bytes;
        bytes.clear();
        sipi::write_jpeg(gray, collect, {90, sipi::JpegSampling::yuv420});
        require(bytes == gray_bytes, "Sampling never changes grayscale JPEG bytes");
        (void)decode(bytes, true);
        bool invalid_sampling = false;
        try { sipi::write_jpeg(image, collect, {90, static_cast<sipi::JpegSampling>(99)}); }
        catch (const sipi::JpegWriteError&) { invalid_sampling = true; }
        require(invalid_sampling, "Invalid sampling rejected");
        for (int quality : {0, 101}) {
            bool failed = false;
            try { sipi::write_jpeg(image, collect, {quality}); } catch (const sipi::JpegWriteError&) { failed = true; }
            require(failed, "invalid quality rejected");
        }
        for (const auto& bad : {sipi::RasterImage8{0, 1, {}, {}}, sipi::RasterImage8{65501, 1, {}, {}},
                               sipi::RasterImage8{1, 1, {0, 0}, {}}}) {
            bool failed = false;
            try { sipi::write_jpeg(bad, collect); } catch (const sipi::JpegWriteError&) { failed = true; }
            require(failed, "invalid raster rejected");
        }
        for (const bool conflict : {false,true}) {
            auto invalid = image;
            if (!conflict) invalid.icc_profile.clear();
            invalid.srgb_intent = conflict ? 0 : 4;
            bool failed = false;
            try { sipi::write_jpeg(invalid, collect); } catch (const sipi::JpegWriteError&) { failed = true; }
            require(failed, "invalid color declaration rejected");
        }
        // Large ICC triggers failure during a full block; tiny JPEG fails at final flush.
        for (int attempt = 0; attempt < 2; ++attempt) {
            bool failed = false;
            try { sipi::write_jpeg(image, [](auto) { throw std::logic_error("sink failed"); }); }
            catch (const std::logic_error& error) { failed = std::string(error.what()) == "sink failed"; }
            require(failed, "sink exception preserved");
            image.icc_profile.clear();
            bytes.clear();
            sipi::write_jpeg(image, collect);
            if (argc == 8) {
                resized_gradient(decode(bytes), std::stoul(argv[2]), std::stoul(argv[3]), std::stoul(argv[4]),
                                 std::stoul(argv[5]), std::stoul(argv[6]), std::stoul(argv[7]));
                return 0;
            }
            gradient(decode(bytes), 0, 0, 320, 200);
        }
        std::cout << "JPEG pixel, ICC, block and failure checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
