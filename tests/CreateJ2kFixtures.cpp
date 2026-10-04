#include <grok.h>
#include "IccFixture.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

/** Generate deterministic, lossless JP2/J2K fixtures without external images.
 * Encoder use is test-only and does not expose a JPEG 2000 output API. */
void write_fixture(const std::filesystem::path& path, bool jp2, bool gray, bool offset, bool tagged = false, unsigned precision = 8, bool is_signed = false, bool ycc = false) {
    std::vector<grk_image_comp> components(gray ? 1 : 3);
    for (auto& c : components) {
        c.w = 37; c.h = 23; c.prec = static_cast<uint8_t>(precision); c.sgnd = is_signed; c.dx = c.dy = 1;
        c.x0 = offset ? 3 : 0; c.y0 = offset ? 5 : 0;
    }
    auto destroy_image = [](grk_image* p) { if (p) grk_object_unref(&p->obj); };
    std::unique_ptr<grk_image, decltype(destroy_image)> image(
        grk_image_new(static_cast<uint16_t>(components.size()), components.data(),
                      gray ? GRK_CLRSPC_GRAY : ycc ? GRK_CLRSPC_SYCC : GRK_CLRSPC_SRGB, true), destroy_image);
    if (!image) throw std::runtime_error("Cannot create Grok fixture image");
    image->x0 = offset ? 3 : 0; image->y0 = offset ? 5 : 0;
    image->x1 = image->x0 + 37; image->y1 = image->y0 + 23;
    for (unsigned i = 0; i < image->numcomps; ++i) {
        auto& c = image->comps[i];
        for (unsigned y = 0; y < c.h; ++y)
            for (unsigned x = 0; x < c.w; ++x)
                static_cast<int32_t*>(c.data)[y*c.stride+x] = (x*7+y*11+i*31)%256;
    }
    if (tagged) {
        const auto profile = fixture::icc_header();
        image->meta = grk_image_meta_new();
        if (!image->meta) throw std::runtime_error("Cannot allocate fixture metadata");
        auto& color = image->meta->color;
        color.icc_profile_len = static_cast<uint32_t>(profile.size());
        color.icc_profile_buf = new uint8_t[profile.size()];
        std::copy(profile.begin(), profile.end(), color.icc_profile_buf);
    }
    std::vector<uint8_t> bytes(1024*1024);
    grk_stream_params stream{};
    stream.buf = bytes.data(); stream.buf_len = bytes.size();
    grk_cparameters params{};
    grk_compress_set_default_params(&params);
    params.cod_format = jp2 ? GRK_FMT_JP2 : GRK_FMT_J2K;
    params.numresolution = 3;
    params.tile_size_on = true; params.t_width = 32; params.t_height = 16;
    const std::unique_ptr<grk_object, decltype(&grk_object_unref)> codec(
        grk_compress_init(&stream, &params, image.get()), grk_object_unref);
    if (!codec) throw std::runtime_error("Cannot initialize fixture encoding");
    const auto size = grk_compress(codec.get(), nullptr);
    if (!size || size > bytes.size()) throw std::runtime_error("Cannot encode fixture");
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(size));
    if (!file) throw std::runtime_error("Cannot write fixture");
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    grk_initialize(nullptr, 1, nullptr);
    try {
        const std::filesystem::path root(argv[1]);
        std::filesystem::create_directories(root);
        write_fixture(root/"source.jp2", true, false, false);
        write_fixture(root/"source.j2k", false, true, false);
        write_fixture(root/"offset.jp2", true, false, true);
        write_fixture(root/"profile.jp2", true, false, false, true);
        write_fixture(root/"rgb-unknown.j2k", false, false, false);
        write_fixture(root/"sixteen.jp2", true, false, false, false, 16);
        write_fixture(root/"signed.jp2", true, false, false, false, 16, true);
        write_fixture(root/"ycc.jp2", true, false, false, false, 8, false, true);
        // Corrupt only the final tile's SOD marker, preserving the main header
        // and file length. A crop wholly inside the first tile should still work.
        std::ifstream input(root/"source.jp2",std::ios::binary);
        std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(input),{}};
        // Legal optional UUID-info metadata that the pinned Grok version skips.
        // Keep fixture pixels unchanged and verify that this warning is benign.
        auto metadata_bytes = bytes;
        std::vector<uint8_t> box;
        const auto word = [&](uint32_t value) {
            for (int shift=24;shift>=0;shift-=8) box.push_back(static_cast<uint8_t>(value>>shift));
        };
        const std::string url = "https://example.invalid/metadata";
        word(static_cast<uint32_t>(8+26+12+url.size()+1));
        box.insert(box.end(),{'u','i','n','f'});
        word(26); box.insert(box.end(),{'u','l','s','t',0,1});
        for (unsigned i=0;i<16;++i) box.push_back(static_cast<uint8_t>(i));
        word(static_cast<uint32_t>(12+url.size()+1));
        box.insert(box.end(),{'u','r','l',' ',0,0,0,0});
        box.insert(box.end(),url.begin(),url.end()); box.push_back(0);
        const std::vector<uint8_t> codestream_type{'j','p','2','c'};
        const auto position = std::search(metadata_bytes.begin(),metadata_bytes.end(),
            codestream_type.begin(),codestream_type.end());
        if (position == metadata_bytes.end()) throw std::runtime_error("Missing JP2 codestream box");
        metadata_bytes.insert(position-4,box.begin(),box.end());
        std::ofstream with_metadata(root/"metadata-box.jp2",std::ios::binary);
        with_metadata.write(reinterpret_cast<const char*>(metadata_bytes.data()),
                            static_cast<std::streamsize>(metadata_bytes.size()));
        std::size_t last_tile = 0;
        for (std::size_t i=0;i+1<bytes.size();++i)
            if (bytes[i]==0xff && bytes[i+1]==0x90) last_tile=i;
        bool changed = false;
        for (std::size_t i=last_tile;i+1<bytes.size();++i)
            if (bytes[i]==0xff && bytes[i+1]==0x93) { bytes[i+1]=0xff; changed=true; break; }
        if (!changed) throw std::runtime_error("Cannot locate test tile marker");
        std::ofstream corrupt(root/"corrupt-tile.jp2",std::ios::binary);
        corrupt.write(reinterpret_cast<const char*>(bytes.data()),static_cast<std::streamsize>(bytes.size()));
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n'; grk_deinitialize(); return 1;
    }
    grk_deinitialize();
}
