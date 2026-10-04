#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>

namespace sipi {

/** Storage failures stay distinct from invalid image/codec data. Diagnostics
 * must not disclose credentials or storage paths to callers of public APIs. */
enum class ByteSourceErrorCode { invalid_argument, not_found, io, changed };
class ByteSourceError : public std::runtime_error {
public:
    ByteSourceError(ByteSourceErrorCode code, const char* message)
        : std::runtime_error(message), code_(code) {}
    [[nodiscard]] ByteSourceErrorCode code() const noexcept { return code_; }
private:
    ByteSourceErrorCode code_;
};

/** Immutable description of the opened source, not a public IIIF service URL.
 * identity and version are opaque backend-qualified strings. A version is a
 * validator under the backend's update contract, not necessarily a content hash.
 * Size is the byte length of this version. No credentials belong in these fields.
 */
struct SourceInfo {
    std::string identity;
    std::string version;
    std::uint64_t size = 0;
};

/** Read-only, version-bound random-access source, independent of codecs/HTTP.
 * Owners keep the source alive until all readers finish. Concurrent const calls
 * must be safe with separate destination buffers; there is no shared seek cursor.
 * Destruction must not race reads. Implementations own their handles/connections
 * and keep info unchanged throughout their lifetime.
 *
 * Implementations must preserve one source version or report a change/failure,
 * never silently reopen a newer version. Local files require no in-place writes;
 * remote implementations must bind requests to a version/conditional validator.
 */
class ByteSource {
public:
    virtual ~ByteSource() = default;
    [[nodiscard]] virtual const SourceInfo& info() const noexcept = 0;

    /** Fill destination up to the captured EOF; return the number of bytes read.
     * At/beyond EOF or for an empty buffer, return zero. A short return is allowed
     * only at the captured EOF, not for partial transport reads. Offset arithmetic
     * must not overflow. No allocation proportional to the object size is implied.
     * Throw ByteSourceError for storage failures; allocation failures may propagate.
     * After any exception, buffer contents are unspecified and must be discarded.
     */
    virtual std::size_t read_at(std::uint64_t offset, std::span<std::byte> destination) const = 0;
};

} // namespace sipi
