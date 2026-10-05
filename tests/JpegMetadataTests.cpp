#include "imgformats/JpegReader.h"
#include "cache/MetadataCache.h"
#include "IccFixture.h"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <jpeglib.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <utility>

namespace {
using namespace sipi;
using Bytes = std::vector<std::uint8_t>;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct Memory final : ByteSource {
    Bytes bytes;
    SourceInfo facts{"memory:jpeg", "v1", 0};
    mutable std::size_t read_bytes = 0;
    bool storage_failure = false;
    explicit Memory(Bytes value) : bytes(std::move(value)) { facts.size = bytes.size(); }
    const SourceInfo& info() const noexcept override { return facts; }
    std::size_t read_at(std::uint64_t offset, std::span<std::byte> target) const override {
        if (storage_failure) throw ByteSourceError(ByteSourceErrorCode::changed, "changed");
        if (offset >= bytes.size()) return 0;
        const auto count = std::min(target.size(), bytes.size() - static_cast<std::size_t>(offset));
        std::copy_n(reinterpret_cast<const std::byte*>(bytes.data()) + offset, count, target.data());
        read_bytes += count;
        return count;
    }
};
/** Test encoder only: exercise actual baseline/progressive/gray/CMYK headers. */
Bytes encoded(J_COLOR_SPACE color = JCS_RGB, bool progressive = false, bool direct_rgb = false, bool ycck = false, bool multiscan = false) {
    jpeg_compress_struct codec{};
    jpeg_error_mgr error{};
    codec.err = jpeg_std_error(&error);
    jpeg_create_compress(&codec);
    unsigned char* buffer = nullptr;
    unsigned long size = 0;
    jpeg_mem_dest(&codec, &buffer, &size);
    codec.image_width = 37; codec.image_height = 23;
    codec.input_components = color == JCS_GRAYSCALE ? 1 : color == JCS_CMYK ? 4 : 3;
    codec.in_color_space = color;
    jpeg_set_defaults(&codec);
    if (direct_rgb) jpeg_set_colorspace(&codec, JCS_RGB);
    if (ycck) jpeg_set_colorspace(&codec, JCS_YCCK);
    if (progressive) jpeg_simple_progression(&codec);
    // Sequential non-interleaved scans also retain full coefficient arrays.
    std::array<jpeg_scan_info, 3> scans{};
    if (multiscan) {
        for (int i = 0; i < 3; ++i) {
            scans[i].comps_in_scan = 1;
            scans[i].component_index[0] = i;
            scans[i].Se = 63;
        }
        codec.scan_info = scans.data();
        codec.num_scans = 3;
    }
    jpeg_start_compress(&codec, TRUE);
    std::array<std::uint8_t, 37 * 4> row{};
    while (codec.next_scanline < codec.image_height) {
        for (std::size_t i = 0; i < row.size(); ++i) row[i] = static_cast<std::uint8_t>(i + codec.next_scanline);
        JSAMPROW pointer = row.data(); jpeg_write_scanlines(&codec, &pointer, 1);
    }
    jpeg_finish_compress(&codec);
    Bytes result(buffer, buffer + size);
    jpeg_destroy_compress(&codec);
    std::free(buffer);
    return result;
}
/** Independent memory-source reference for valid fixtures only. */
Bytes reference_pixels(const Bytes& bytes) {
    jpeg_decompress_struct codec{};
    jpeg_error_mgr error{};
    codec.err = jpeg_std_error(&error);
    jpeg_create_decompress(&codec);
    jpeg_mem_src(&codec, bytes.data(), bytes.size());
    jpeg_read_header(&codec, TRUE);
    codec.out_color_space = codec.num_components == 1 ? JCS_GRAYSCALE : JCS_RGB;
    codec.dct_method = JDCT_ISLOW;
    codec.do_fancy_upsampling = TRUE;
    jpeg_start_decompress(&codec);
    Bytes pixels(std::size_t(codec.output_width) * codec.output_height * codec.output_components);
    while (codec.output_scanline < codec.output_height) {
        JSAMPROW row = pixels.data() + std::size_t(codec.output_scanline) * codec.output_width * codec.output_components;
        jpeg_read_scanlines(&codec, &row, 1);
    }
    jpeg_finish_decompress(&codec);
    jpeg_destroy_decompress(&codec);
    return pixels;
}
Bytes marker(unsigned type, const Bytes& value) {
    check(value.size() <= 65533, "test marker too large");
    const auto size = value.size() + 2;
    Bytes result{255, static_cast<std::uint8_t>(type), static_cast<std::uint8_t>(size >> 8), static_cast<std::uint8_t>(size)};
    result.insert(result.end(), value.begin(), value.end());
    return result;
}
Bytes insert(Bytes jpeg, const Bytes& app) { jpeg.insert(jpeg.begin() + 2, app.begin(), app.end()); return jpeg; }
Bytes exif(unsigned value, bool big = false) {
    Bytes result{'E','x','i','f',0,0};
    const Bytes little{'I','I',42,0,8,0,0,0,1,0,0x12,1,3,0,1,0,0,0,static_cast<std::uint8_t>(value),0,0,0,0,0,0,0};
    const Bytes large{'M','M',0,42,0,0,0,8,0,1,1,0x12,0,3,0,0,0,1,0,static_cast<std::uint8_t>(value),0,0,0,0,0,0};
    const auto& data = big ? large : little;
    result.insert(result.end(), data.begin(), data.end());
    return marker(0xe1, result);
}
Bytes icc(unsigned sequence, unsigned total, const Bytes& data) {
    Bytes value{'I','C','C','_','P','R','O','F','I','L','E',0,
                static_cast<std::uint8_t>(sequence), static_cast<std::uint8_t>(total)};
    value.insert(value.end(), data.begin(), data.end());
    return marker(0xe2, value);
}
template<class Error = JpegReadError> void rejects(Bytes bytes) {
    bool failed = false;
    try { (void)read_jpeg_metadata(Memory(std::move(bytes))); }
    catch (const Error&) { failed = true; }
    check(failed, "invalid JPEG accepted");
}
void save(const std::filesystem::path& directory, const char* name, const Bytes& bytes) {
    std::ofstream file(directory / name, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    check(bool(file), "fixture write failed");
}
void run(const std::filesystem::path& directory) {
    std::filesystem::create_directories(directory);
    const auto base = encoded();
    const auto gray = encoded(JCS_GRAYSCALE);
    const auto progressive = encoded(JCS_RGB, true);
    const auto cmyk = encoded(JCS_CMYK);
    const auto sequential = encoded(JCS_RGB, false, false, false, true);
    for (const auto& bytes : {base, gray, progressive}) {
        const auto metadata = read_jpeg_metadata(Memory(bytes));
        check(metadata.width == 37 && metadata.height == 23, "SOF dimensions");
    }
    check(read_jpeg_metadata(Memory(gray)).color == JpegColor::gray, "gray layout");
    check(read_jpeg_metadata(Memory(progressive)).progressive, "progressive flag");
    check(!read_jpeg_metadata(Memory(base)).progressive, "baseline flag");
    rejects<JpegUnsupportedError>(cmyk);
    rejects<JpegUnsupportedError>(encoded(JCS_CMYK, false, false, true));
    check(read_jpeg_metadata(Memory(encoded(JCS_RGB, false, true))).color == JpegColor::rgb, "direct RGB header");
    auto twelve = base;
    for (std::size_t at = 2; at + 4 < twelve.size();) {
        if (twelve[at + 1] == 0xc0) { twelve[at + 4] = 12; break; }
        at += 2 + std::size_t(twelve[at + 2]) * 256 + twelve[at + 3];
    }
    rejects<JpegUnsupportedError>(twelve);
    for (bool big : {false, true}) {
        (void)read_jpeg_metadata(Memory(insert(base, exif(1, big))));
        for (unsigned value = 2; value <= 8; ++value) rejects<JpegUnsupportedError>(insert(base, exif(value, big)));
        rejects(insert(base, exif(0, big)));
        rejects(insert(base, exif(9, big)));
    }
    rejects(insert(insert(base, exif(1)), exif(1)));
    auto bad_offset = exif(1); bad_offset[14] = 255; // TIFF IFD0 offset exceeds payload.
    rejects(insert(base, bad_offset));
    rejects(insert(base, marker(0xe1, {'E','x','i','f',0,0,'I','I'})));
    (void)read_jpeg_metadata(Memory(insert(base, marker(0xe1, {'X','M','P'}))));
    const Bytes first(65000, 42), second{1,2,3,4};
    const auto tagged = insert(insert(base, icc(1, 2, first)), icc(2, 2, second));
    auto expected = first; expected.insert(expected.end(), second.begin(), second.end());
    const auto metadata = read_jpeg_metadata(Memory(tagged));
    check(metadata.icc_profile == expected, "out-of-order ICC assembly");
    rejects(insert(base, icc(1, 2, first)));
    rejects(insert(insert(base, icc(1, 1, first)), icc(1, 1, second)));
    rejects(insert(insert(base, icc(1, 2, first)), icc(2, 3, second)));
    rejects(insert(base, icc(0, 1, second)));
    rejects(insert(base, icc(2, 1, second)));
    rejects(insert(base, icc(1, 1, {})));
    for (std::size_t size : {0u, 2u, 10u, 30u}) rejects(Bytes(base.begin(), base.begin() + size));
    bool limited = false;
    try { (void)read_jpeg_metadata(Memory(tagged), 100); } catch (const JpegReadError&) { limited = true; }
    check(limited, "input budget ignored");
    Memory failing(base); failing.storage_failure = true;
    bool changed = false;
    try { (void)read_jpeg_metadata(failing); }
    catch (const ByteSourceError& error) { changed = error.code() == ByteSourceErrorCode::changed; }
    check(changed, "storage error translated to codec error");
    auto large = base; large.resize(1024 * 1024, 0);
    Memory bounded(std::move(large)); (void)read_jpeg_metadata(bounded);
    check(bounded.read_bytes <= 16384, "header read staged the whole source");
    // Headers do not validate the entropy stream: even an SOS-only file is
    // readable here. Pixel delivery must later validate/decode independently.
    auto header = base;
    for (std::size_t at = 2; at + 4 < header.size();) {
        const auto length = std::size_t(header[at + 2]) * 256 + header[at + 3];
        if (header[at + 1] == 0xda) { header.resize(at + 2 + length); break; }
        at += 2 + length;
    }
    (void)read_jpeg_metadata(Memory(header));
    for (const auto& bytes : {base, gray, progressive, sequential, tagged, encoded(JCS_RGB, false, true)}) {
        Memory source(bytes);
        const auto facts = read_jpeg_metadata(source);
        const auto image = read_jpeg_image(source, facts);
        check(image.width == 37 && image.height == 23, "decoded dimensions");
        check(image.pixels == reference_pixels(bytes), "decoded samples differ from memory-source reference");
        check(image.icc_profile == facts.icc_profile, "decoded ICC lost");
    }
    const auto rejects_pixels = [](const Bytes& bytes, std::size_t memory, std::size_t input) {
        Memory source(bytes);
        const auto facts = read_jpeg_metadata(source);
        bool failed = false;
        try { (void)read_jpeg_image(source, facts, memory, input); }
        catch (const JpegReadError&) { failed = true; }
        check(failed, "invalid or oversized pixel decode accepted");
    };
    rejects_pixels(header, 1000000, 1000000);
    rejects_pixels(Bytes(base.begin(), base.end() - 2), 1000000, 1000000);
    rejects_pixels(base, 37 * 23 * 3 - 1, 1000000);
    rejects_pixels(progressive, 37 * 23 * 3, 1000000); // Coefficients also require space.
    rejects_pixels(sequential, 37 * 23 * 3, 1000000);
    rejects_pixels(tagged, 37 * 23 * 3, 1000000); // ICC also requires space.
    rejects_pixels(base, 1000000, base.size() - 2);
    auto wrong = read_jpeg_metadata(Memory(base)); ++wrong.width;
    bool mismatch = false;
    try { (void)read_jpeg_image(Memory(base), wrong); }
    catch (const JpegReadError&) { mismatch = true; }
    check(mismatch, "stale metadata accepted");
    changed = false;
    try { (void)read_jpeg_image(failing, read_jpeg_metadata(Memory(base))); }
    catch (const ByteSourceError& error) { changed = error.code() == ByteSourceErrorCode::changed; }
    check(changed, "pixel reader swallowed storage error");
    std::vector<std::future<void>> tasks;
    for (unsigned i = 0; i < 8; ++i) tasks.push_back(std::async(std::launch::async, [&] {
        check(read_jpeg_image(Memory(tagged), metadata).icc_profile == expected, "parallel pixel read");
    }));
    for (auto& task : tasks) task.get();
    cache::MetadataCache cache;
    auto snapshot = std::make_shared<const cache::ImageMetadata>(metadata);
    check(cache.put({"jpeg", "v1", tagged.size()}, snapshot), "JPEG cache admission");
    check(std::get<JpegMetadata>(*cache.get({"jpeg", "v1", tagged.size()})).icc_profile == expected, "cached ICC");
    check(cache.resident_bytes() >= expected.size(), "JPEG ICC accounting");
    check(!cache::MetadataCache({10,100}).put({"jpeg", "v1", tagged.size()}, snapshot), "JPEG cache budget");
    save(directory, "rgb.jpg", base); save(directory, "gray.jpg", gray);
    save(directory, "progressive.jpg", progressive); save(directory, "cmyk.jpg", cmyk);
    const auto profile = fixture::icc_header();
    save(directory, "expected.icc", profile);
    save(directory, "icc.jpg", insert(insert(base, icc(1, 2, Bytes(profile.begin(), profile.begin()+64))),
                                      icc(2, 2, Bytes(profile.begin()+64, profile.end())))); save(directory, "orientation.jpg", insert(base, exif(6)));
    save(directory, "orientation-one.jpg", insert(base, exif(1, true)));
    save(directory, "broken.jpg", Bytes(base.begin(), base.begin() + 30));
    save(directory, "header-only.jpg", header);
}
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try { run(argv[1]); }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    std::cout << "JPEG header, ICC, EXIF, storage, concurrency and cache checks passed\n";
}
