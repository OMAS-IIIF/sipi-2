#pragma once

#include "ByteSource.h"
#include <filesystem>

namespace sipi {

/** POSIX local-file source (macOS/Linux), opened once and closed through RAII.
 * pread gives independent concurrent offset reads. Atomic path replacement or
 * unlink does not redirect existing readers; in-place writes are prohibited.
 * Identity uses device/inode; the captured version adds size, mtime and ctime
 * at nanosecond precision. This is a local fingerprint, not a cryptographic hash.
 * Constructor requires a regular file and throws ByteSourceError on open/status
 * failure. It does not authorize paths: use SourceResolver first for HTTP input.
 */
class LocalFileSource final : public ByteSource {
public:
    explicit LocalFileSource(const std::filesystem::path& path);
    ~LocalFileSource() override = default;
    LocalFileSource(const LocalFileSource&) = delete;
    LocalFileSource& operator=(const LocalFileSource&) = delete;

    [[nodiscard]] const SourceInfo& info() const noexcept override { return info_; }
    std::size_t read_at(std::uint64_t offset, std::span<std::byte> destination) const override;

    /** Canonical path of the file behind the open descriptor, as the kernel
     * reports it now (Linux /proc/self/fd, macOS F_GETPATH). Unlike the path
     * given to the constructor, it cannot be redirected by later renames or
     * symlink swaps of the directories that were traversed by open().
     * Throws ByteSourceError(changed) if the file has been unlinked, and
     * ByteSourceError(io) if the platform cannot report the path. */
    [[nodiscard]] std::filesystem::path opened_path() const;

private:
    /** Member ownership also closes the descriptor if construction throws. */
    struct Descriptor {
        int value = -1;
        ~Descriptor();
    } descriptor_;
    SourceInfo info_;
};

} // namespace sipi
