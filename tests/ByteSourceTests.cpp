#include "storage/LocalFileSource.h"
#include "imgformats/TiffReader.h"
#include <tiffio.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sys/stat.h>

namespace {
using namespace sipi;
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class Function>
void rejects(Function action, ByteSourceErrorCode code) {
    try { action(); } catch (const ByteSourceError& error) {
        require(error.code() == code, "unexpected byte-source error code");
        return;
    }
    throw std::runtime_error("expected byte-source error");
}
void write_bytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream file(path, std::ios::binary);
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    file.close();
    require(bool(file), "cannot write test source");
}
std::string read_text(const ByteSource& source, std::uint64_t offset, std::size_t count) {
    std::string bytes(count, '\0');
    bytes.resize(source.read_at(offset, std::as_writable_bytes(std::span{bytes})));
    return bytes;
}

/** Real TIFFs use independent fixture generation, including a sparse BigTIFF
 * whose tile and directory offsets exceed 4 GiB without allocating a huge image. */
void write_tiff(const std::filesystem::path& path, unsigned width, unsigned char value, bool sparse = false) {
    std::unique_ptr<TIFF, decltype(&TIFFClose)> file(TIFFOpen(path.c_str(), sparse ? "w8" : "w"), TIFFClose);
    require(bool(file), "cannot create test TIFF");
    TIFFSetField(file.get(), TIFFTAG_IMAGEWIDTH, width);
    TIFFSetField(file.get(), TIFFTAG_IMAGELENGTH, 16);
    TIFFSetField(file.get(), TIFFTAG_SAMPLESPERPIXEL, 3);
    TIFFSetField(file.get(), TIFFTAG_BITSPERSAMPLE, 8);
    TIFFSetField(file.get(), TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
    TIFFSetField(file.get(), TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
    TIFFSetField(file.get(), TIFFTAG_COMPRESSION, COMPRESSION_NONE);
    TIFFSetField(file.get(), TIFFTAG_TILEWIDTH, 16);
    TIFFSetField(file.get(), TIFFTAG_TILELENGTH, 16);
    // The writer appends its first tile at EOF. Reserve a sparse gap first.
    if (sparse) std::filesystem::resize_file(path, (std::uint64_t{1} << 32) + 128);
    std::array<unsigned char, 16 * 16 * 3> tile;
    tile.fill(value);
    for (unsigned x = 0; x < width; x += 16)
        require(TIFFWriteEncodedTile(file.get(), x / 16, tile.data(), tile.size()) > 0, "cannot write test tile");
}
void pixels(const ByteSource& source, unsigned char value) {
    const auto crop = read_tiff_region(source, {1, 2, 8, 9});
    require(crop.width == 8 && crop.height == 9 && crop.pixels.size() == 8*9*3, "crop dimensions");
    require(std::all_of(crop.pixels.begin(), crop.pixels.end(), [value](auto b) { return b == value; }), "crop content");
}

/** In-memory test backend proves codecs have no path dependency. Optional fault
 * injection exercises C-callback exception recovery; mutex protects read tracing.
 * Deliberately short reads violate the interface to test the adapter's guard.
 */
class MemorySource final : public ByteSource {
public:
    enum class Fault { none, header, tile, short_read, allocation };
    MemorySource(const ByteSource& source, Fault fault = Fault::none, std::uint64_t tile_offset = 0)
        : bytes_(source.info().size), fault_(fault), tile_offset_(tile_offset) {
        require(source.read_at(0, bytes_) == bytes_.size(), "test source load");
        info_ = {"test-memory:fixture", "v1", bytes_.size()};
    }
    const SourceInfo& info() const noexcept override { return info_; }
    std::size_t read_at(std::uint64_t offset, std::span<std::byte> output) const override {
        std::lock_guard lock(mutex_);
        if (offset >= bytes_.size() || output.empty()) return 0;
        auto count = static_cast<std::size_t>(std::min<std::uint64_t>(output.size(), bytes_.size() - offset));
        if (fault_ == Fault::allocation) throw std::bad_alloc{};
        if (fault_ == Fault::header || (fault_ == Fault::tile && offset <= tile_offset_ && count > tile_offset_ - offset))
            throw ByteSourceError(ByteSourceErrorCode::io, "Injected storage failure");
        if (fault_ == Fault::short_read) --count;
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset), count, output.begin());
        ++reads_;
        return count;
    }
    std::size_t reads() const { std::lock_guard lock(mutex_); return reads_; }
private:
    std::vector<std::byte> bytes_;
    SourceInfo info_;
    Fault fault_;
    std::uint64_t tile_offset_;
    mutable std::mutex mutex_;
    mutable std::size_t reads_ = 0;
};

void local_contract(const std::filesystem::path& root) {
    const auto path = root / "bytes.bin";
    std::string original(10000, '\0');
    for (std::size_t i = 0; i < original.size(); ++i) original[i] = static_cast<char>(i % 251);
    write_bytes(path, original);
    const LocalFileSource source(path);
    const auto captured = source.info();
    require(captured.size == original.size() && captured.identity.starts_with("local:") && !captured.version.empty(), "source info");
    require(read_text(source, 700, 80) == original.substr(700, 80), "offset read");
    require(read_text(source, 9997, 10) == original.substr(9997), "read clips at EOF");
    require(read_text(source, captured.size, 10).empty(), "at EOF");
    require(read_text(source, std::numeric_limits<std::uint64_t>::max(), 10).empty(), "beyond EOF without overflow");
    require(read_text(source, 0, 0).empty(), "empty buffer");
    std::vector<std::future<bool>> jobs;
    for (unsigned n = 0; n < 8; ++n) jobs.push_back(std::async(std::launch::async, [&, n] {
        for (unsigned i = 0; i < 100; ++i) {
            const auto offset = (n * 79 + i * 67) % 9900;
            if (read_text(source, offset, 100) != original.substr(offset, 100)) return false;
        }
        return true;
    }));
    for (auto& job : jobs) require(job.get(), "concurrent offset reads");
    const auto replacement = root / "replacement.bin";
    write_bytes(replacement, "replacement");
    std::filesystem::rename(replacement, path);
    const LocalFileSource newer(path);
    require(newer.info().identity != captured.identity, "replacement has distinct identity");
    require(read_text(source, 0, original.size()) == original, "open source survives replacement");
    require(read_text(newer, 0, 100) == "replacement", "new open sees replacement");
    std::filesystem::remove(path);
    require(read_text(newer, 0, 100) == "replacement", "open source survives unlink");
    require(source.info().identity == captured.identity && source.info().version == captured.version, "info immutable");
    rejects([&] { LocalFileSource missing(path); }, ByteSourceErrorCode::not_found);
    rejects([&] { LocalFileSource empty(""); }, ByteSourceErrorCode::invalid_argument);
    rejects([&] { LocalFileSource nul(std::filesystem::path{std::string("a\0b", 3)}); }, ByteSourceErrorCode::invalid_argument);
    rejects([&] { LocalFileSource directory(root); }, ByteSourceErrorCode::invalid_argument);
    const auto fifo = root / "pipe";
    std::filesystem::remove(fifo);
    require(::mkfifo(fifo.c_str(), 0600) == 0, "create FIFO");
    rejects([&] { LocalFileSource pipe(fifo); }, ByteSourceErrorCode::invalid_argument);
    std::filesystem::remove(fifo);
    write_bytes(path, original);
    const LocalFileSource truncated(path);
    std::filesystem::resize_file(path, 3); // deliberate violation, detected as premature EOF
    rejects([&] { (void)read_text(truncated, 0, 20); }, ByteSourceErrorCode::changed);
    std::filesystem::remove(path);
}

void codec_contract(const std::filesystem::path& root) {
    const auto path = root / "image.tif";
    const auto replacement = root / "new.tif";
    write_tiff(path, 16, 42);
    const LocalFileSource source(path);
    require(read_tiff_metadata(source).image.width == 16, "initial metadata");
    MemorySource memory(source);
    write_tiff(replacement, 32, 99);
    std::filesystem::rename(replacement, path);
    pixels(source, 42); // metadata and pixels still refer to the old open image
    require(read_tiff_metadata(source).image.width == 16, "old metadata after replacement");
    const LocalFileSource newer(path);
    require(read_tiff_metadata(newer).image.width == 32, "new metadata after replacement");
    pixels(newer, 99);
    std::filesystem::remove(path);
    require(read_tiff_metadata(memory).image.width == 16, "metadata from non-file source");
    pixels(memory, 42);
    require(memory.reads() > 1, "codec uses random-access source");
    // Every libtiff handle has its own cursor, even on one shared source.
    std::vector<std::future<void>> jobs;
    for (unsigned n = 0; n < 8; ++n) jobs.push_back(std::async(std::launch::async, [&] {
        require(read_tiff_metadata(source).image.width == 16, "concurrent TIFF metadata");
        pixels(source, 42);
    }));
    for (auto& job : jobs) job.get();
    MemorySource broken_header(source, MemorySource::Fault::header);
    rejects([&] { (void)read_tiff_metadata(broken_header); }, ByteSourceErrorCode::io);
    MemorySource short_read(source, MemorySource::Fault::short_read);
    rejects([&] { (void)read_tiff_metadata(short_read); }, ByteSourceErrorCode::io);
    MemorySource allocation(source, MemorySource::Fault::allocation);
    bool allocation_failed = false;
    try { (void)read_tiff_metadata(allocation); } catch (const std::bad_alloc&) { allocation_failed = true; }
    require(allocation_failed, "allocation exception crosses C callback safely");
    // Our first uncompressed classic TIFF tile starts immediately after its header.
    MemorySource broken_tile(source, MemorySource::Fault::tile, 8);
    require(read_tiff_metadata(broken_tile).image.width == 16, "fault outside metadata ranges");
    rejects([&] { pixels(broken_tile, 42); }, ByteSourceErrorCode::io);
    pixels(source, 42); // failure in one handle must not poison others
    const auto sparse = root / "sparse-big.tif";
    write_tiff(sparse, 16, 77, true);
    {
        const LocalFileSource large(sparse);
        require(large.info().size > (std::uint64_t{1} << 32), "sparse fixture exceeds 32-bit offsets");
        require(read_tiff_metadata(large).image.width == 16, "BigTIFF metadata beyond 4 GiB");
        pixels(large, 77);
    }
    std::filesystem::remove(sparse);
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    try {
        const std::filesystem::path root(argv[1]);
        std::filesystem::create_directories(root);
        local_contract(root);
        codec_contract(root);
        std::cout << "Byte source, replacement, callback failure and shared-reader checks passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
