#include "LocalFileSource.h"

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>

namespace sipi {

LocalFileSource::Descriptor::~Descriptor() {
    // Never retry close after EINTR: the descriptor may already be released.
    if (value >= 0) ::close(value);
}

LocalFileSource::LocalFileSource(const std::filesystem::path& path) {
    const auto name = path.string();
    if (name.empty() || name.find('\0') != std::string::npos)
        throw ByteSourceError(ByteSourceErrorCode::invalid_argument, "Invalid local source path");
    // Nonblocking avoids hanging on a FIFO replaced into the path before open.
    // It has no effect for regular files, the only supported local source type.
    do {
        descriptor_.value = ::open(name.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    } while (descriptor_.value < 0 && errno == EINTR);
    if (descriptor_.value < 0) {
        const auto code = errno == ENOENT || errno == ENOTDIR
            ? ByteSourceErrorCode::not_found : ByteSourceErrorCode::io;
        throw ByteSourceError(code, "Local image source could not be opened");
    }
    struct stat status{};
    int result;
    do { result = ::fstat(descriptor_.value, &status); } while (result < 0 && errno == EINTR);
    if (result < 0) throw ByteSourceError(ByteSourceErrorCode::io, "Local image source status could not be read");
    if (!S_ISREG(status.st_mode))
        throw ByteSourceError(ByteSourceErrorCode::invalid_argument, "Local source must be a regular file");
    if (status.st_size < 0)
        throw ByteSourceError(ByteSourceErrorCode::io, "Invalid local image source size");
#if defined(__APPLE__)
    const auto modified = status.st_mtimespec;
    const auto changed = status.st_ctimespec;
#else
    const auto modified = status.st_mtim;
    const auto changed = status.st_ctim;
#endif
    info_.size = static_cast<std::uint64_t>(status.st_size);
    info_.identity = "local:" + std::to_string(status.st_dev) + ':' + std::to_string(status.st_ino);
    info_.version = "stat-v1:" + std::to_string(info_.size) + ':' +
        std::to_string(modified.tv_sec) + ':' + std::to_string(modified.tv_nsec) + ':' +
        std::to_string(changed.tv_sec) + ':' + std::to_string(changed.tv_nsec);
}

std::size_t LocalFileSource::read_at(std::uint64_t offset, std::span<std::byte> destination) const {
    if (offset >= info_.size || destination.empty()) return 0;
    const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(destination.size(), info_.size - offset));
    std::size_t completed = 0;
    while (completed < wanted) {
        const auto count = std::min(wanted - completed, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        // Offset is below the captured off_t file size; no narrowing overflow.
        const auto read = ::pread(descriptor_.value, destination.data() + completed, count,
                                  static_cast<off_t>(offset + completed));
        if (read < 0) {
            if (errno == EINTR) continue;
            throw ByteSourceError(ByteSourceErrorCode::io, "Local image source read failed");
        }
        if (read == 0)
            throw ByteSourceError(ByteSourceErrorCode::changed, "Local image source ended before its captured size");
        completed += static_cast<std::size_t>(read);
    }
    return completed;
}

} // namespace sipi
