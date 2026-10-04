#pragma once

#include "storage/ByteSource.h"
#include "image/RasterImage.h"
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace sipi {
class J2kReadError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class J2kUnsupportedError : public J2kReadError {
public:
    using J2kReadError::J2kReadError;
};
enum class J2kColor { unknown, gray, rgb, unsupported };

/** Owning component description, before any resampling or color conversion. */
struct J2kComponent {
    std::uint32_t width, height;
    std::uint8_t precision, step_x, step_y;
    bool is_signed;
};

/** Header facts, not a promise that the pixel pipeline supports this source.
 * width/height are reference-grid extents, independent of nonzero grid origins.
 * resolution_count describes the main coding style; tile overrides may differ.
 * No borrowed Grok pointers or decoded pixels escape the reader. */
struct J2kMetadata {
    std::uint32_t width, height;
    std::uint32_t origin_x, origin_y;
    std::uint32_t tile_width, tile_height;
    std::uint8_t resolution_count;
    std::vector<J2kComponent> components;
    std::vector<std::uint8_t> icc_profile;
    J2kColor color = J2kColor::unknown;
    bool mapped_channels = false;
    std::uint32_t tile_origin_x = 0, tile_origin_y = 0;
};

/** Read a raw J2K codestream or JP2 header through a version-bound ByteSource.
 * Borrows source for the call; each call owns its codec and seek cursor, so
 * independent calls may run concurrently. No pixel decompression, pathname
 * reopen, direct network access or full-object staging is performed.
 * A 64 KiB read buffer may prefetch compressed bytes. max_read_bytes bounds
 * cumulative callback input (including rereads), not Grok's internal allocations.
 * Header/limit errors throw J2kReadError; storage exceptions propagate unchanged
 * after leaving C callbacks. ICC is copied unchanged, limited to 16 MiB.
 * Grok's process runtime is initialized once, with one codec worker, and shut
 * down at process exit; callers must finish all reads before static teardown. */
[[nodiscard]] J2kMetadata read_j2k_metadata(const ByteSource& source,
    std::size_t max_read_bytes = 16 * 1024 * 1024);
/** Metadata-level pixel capability, not validation of compressed data.
 * Only unsigned 8-bit co-sited gray/RGB without palette/channel remapping.
 * Untagged one-component codestreams mean grayscale by repository convention;
 * untagged multicomponent data needs explicit JP2 color information. */
[[nodiscard]] bool supports_j2k_pixels(const J2kMetadata& metadata) noexcept;
struct J2kReadPlan;
/** Decode the planned region/resolution from the same opened source version.
 * Source metadata is rechecked. Returns owning packed samples and unchanged ICC.
 * No color conversion, alpha, orientation or final resizing. Input limit is
 * 256 MiB cumulative callback bytes; budget bounds output plus a conservative
 * int32 tile-sample estimate, NOT all codec scratch allocations or process RSS.
 * Each call uses a synchronous codec-local executor; callbacks cannot race.
 * Unsupported layouts throw J2kUnsupportedError, corruption/limits J2kReadError;
 * storage exceptions propagate unchanged. Codec errors and data warnings are fatal,
 * including partial tile success. The pinned unknown-box-skipping warning is
 * allowed for extensible JP2 metadata. SIPI owns Grok's process-global log handlers;
 * synchronous requests route diagnostics to independent thread-local scopes. */
[[nodiscard]] RasterImage8 read_j2k_region(const ByteSource& source,
    const J2kMetadata& metadata, const J2kReadPlan& plan,
    std::size_t max_decoded_bytes = 256 * 1024 * 1024);
} // namespace sipi
