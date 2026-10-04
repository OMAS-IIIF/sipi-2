#include "imgformats/J2kReader.h"
#include "IccFixture.h"
#include "storage/LocalFileSource.h"
#include <algorithm>
#include <filesystem>
#include <future>
#include <fstream>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

/** Instrument a source without changing its immutable identity or contents. */
class ObservedSource final : public sipi::ByteSource {
public:
    explicit ObservedSource(const sipi::ByteSource& source) : source_(source) {}
    const sipi::SourceInfo& info() const noexcept override { return source_.info(); }
    std::size_t read_at(std::uint64_t offset, std::span<std::byte> data) const override {
        if (fail) throw sipi::ByteSourceError(sipi::ByteSourceErrorCode::changed, "test source changed");
        bytes += data.size();
        return source_.read_at(offset, data);
    }
    bool fail = false;
    mutable std::size_t bytes = 0;
private:
    const sipi::ByteSource& source_;
};

/** A tiny real codestream at the start of a virtual 1 GiB source. The tail is
 * synthesized, so the test can prove bounded prefetch without allocating it. */
class LargeSource final : public sipi::ByteSource {
public:
    explicit LargeSource(const sipi::ByteSource& source) : header(source.info().size) {
        source.read_at(0, header);
    }
    const sipi::SourceInfo& info() const noexcept override { return description; }
    std::size_t read_at(std::uint64_t offset, std::span<std::byte> data) const override {
        if (offset >= description.size) return 0;
        const auto size = static_cast<std::size_t>(std::min<std::uint64_t>(data.size(), description.size-offset));
        std::fill_n(data.begin(), size, std::byte{});
        if (offset < header.size())
            std::memcpy(data.data(), header.data()+offset, std::min(size, header.size()-static_cast<std::size_t>(offset)));
        bytes += size;
        return size;
    }
    mutable std::size_t bytes = 0;
private:
    sipi::SourceInfo description{"test:large", "v1", 1024ULL*1024*1024};
    std::vector<std::byte> header;
};

int main(int argc, char** argv) {
    try {
        require(argc == 2, "fixture directory required");
        const std::filesystem::path root(argv[1]);
        for (const auto* name : {"source.jp2", "source.j2k", "offset.jp2", "metadata-box.jp2"}) {
            sipi::LocalFileSource file(root/name);
            ObservedSource source(file);
            const auto m = sipi::read_j2k_metadata(source);
            require(m.width == 37 && m.height == 23, "reference-grid dimensions");
            require(m.tile_width == 32 && m.tile_height == 16, "tile dimensions");
            require(m.resolution_count == 3, "resolution count");
            require(m.components.size() == (std::string(name) == "source.j2k" ? 1 : 3), "components");
            for (const auto& c : m.components)
                require(c.width == 37 && c.height == 23 && c.precision == 8 &&
                        !c.is_signed && c.step_x == 1 && c.step_y == 1, "component facts");
            require(m.origin_x == (std::string(name) == "offset.jp2" ? 3 : 0), "origin");
            require(source.bytes > 0 && source.bytes <= file.info().size * 2, "bounded callback reads");
            bool caught = false;
            try { (void)sipi::read_j2k_metadata(source, 1); }
            catch (const sipi::J2kReadError&) { caught = true; }
            require(caught, "header budget enforced");
            source.fail = true; caught = false;
            try { (void)sipi::read_j2k_metadata(source); }
            catch (const sipi::ByteSourceError& e) { caught = e.code() == sipi::ByteSourceErrorCode::changed; }
            require(caught, "source error propagated across callback");
        }
        sipi::LocalFileSource tagged(root/"profile.jp2");
        require(sipi::read_j2k_metadata(tagged).icc_profile == fixture::icc_header(),
                "ICC bytes remain owned and unchanged after codec destruction");
        sipi::LocalFileSource raw(root/"source.j2k");
        LargeSource large(raw);
        require(sipi::read_j2k_metadata(large).height == 23, "large source header");
        require(large.bytes <= 128*1024, "header must not stage whole object");
        for (const auto* name : {"source.jp2", "source.j2k"}) {
            std::ifstream input(root/name, std::ios::binary);
            char bytes[32]{}; input.read(bytes, sizeof(bytes));
            const auto path = root/(std::string(name)+".truncated");
            { std::ofstream output(path, std::ios::binary); output.write(bytes, sizeof(bytes)); }
            sipi::LocalFileSource truncated(path);
            bool caught = false;
            try { (void)sipi::read_j2k_metadata(truncated); }
            catch (const sipi::J2kReadError&) { caught = true; }
            require(caught, "truncated header must fail");
        }
        sipi::LocalFileSource shared(root/"source.jp2");
        std::vector<std::future<sipi::J2kMetadata>> jobs;
        for (int i = 0; i < 8; ++i)
            jobs.push_back(std::async(std::launch::async, [&] { return sipi::read_j2k_metadata(shared); }));
        for (auto& job : jobs) require(job.get().width == 37, "independent concurrent cursors");
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
