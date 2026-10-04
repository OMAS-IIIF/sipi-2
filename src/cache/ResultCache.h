#pragma once

#include "CacheKey.h"
#include "storage/ByteSource.h"
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>

namespace sipi::cache {

/** Encoded file budget, including the one possible in-progress file. Database,
 * journal and filesystem overhead are additional; leave free disk headroom.
 * Oversized entries and admissions blocked by active readers are declined. */
struct CacheLimits {
    std::uint64_t max_bytes = 1024ULL * 1024 * 1024;
    unsigned target_percent = 90; // 1..99; cleanup target after reaching the limit
};

class CacheError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/** Facts about the encoded output, not the original image or HTTP headers. */
struct ResultMetadata {
    std::string media_type;
    std::uint32_t width = 0, height = 0;
    bool operator==(const ResultMetadata&) const = default;
};

/** An owning file handle. Reads remain valid after ResultCache destruction.
 * Retaining bytes pins the entry against eviction while this cache is open.
 * ByteSource supports independent concurrent offset reads without a seek cursor. */
struct CachedResult {
    ResultMetadata metadata;
    std::shared_ptr<const ByteSource> bytes;
};

/** Local, persistent encoded-result store, for one instance per directory.
 * Creates cache.sqlite3, cache.lock and objects/<hash-prefix>/<hash>.bin.
 * An OS lock excludes other instances/processes; use a private local directory,
 * never the image repository, shared network storage or an untrusted directory.
 *
 * Constructor reconciles abandoned files/missing rows before admitting work.
 * put accepts only a COMPLETE successful encoding, writes a temporary file,
 * synchronizes/closes/renames it, then commits the ready SQLite row. The caller
 * owns encoding and authorization; this class does not interpret image contents.
 * All methods serialize index/file mutations with one mutex; returned file reads
 * run independently. No decoding/encoding runs under this mutex.
 *
 * Cache misses return nullopt. Capacity/headroom rejections return false. I/O,
 * database and collision failures throw CacheError (filesystem/allocation errors
 * may also propagate). A failed put disables further writes until reopening, so
 * an uncertain leftover file cannot escape accounting. Callers should continue
 * uncached delivery. Destruction never throws; pending last-use updates are best
 * effort. Do not race destruction with method calls. HTTP integration lives in
 * IiifWorker; this storage class never makes authorization decisions.
 */
class ResultCache {
public:
    explicit ResultCache(const std::filesystem::path& directory, CacheLimits limits = {});
    ~ResultCache();
    ResultCache(const ResultCache&) = delete;
    ResultCache& operator=(const ResultCache&) = delete;

    [[nodiscard]] std::optional<CachedResult> get(const CacheKey& key);
    /** true means stored or already present with identical identity/metadata.
     * An existing entry is never overwritten. Empty encodings are invalid. */
    [[nodiscard]] bool put(const CacheKey& key, const ResultMetadata& metadata,
                           std::span<const std::byte> bytes);
    /** Flush approximate LRU timestamps, also done after 64 hits and on eviction. */
    void flush_accesses();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace sipi::cache
