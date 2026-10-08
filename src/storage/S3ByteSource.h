#pragma once

#include "ByteSource.h"
#include <memory>

namespace sipi {

/** Trusted backend configuration, independent of public IIIF URLs.
 * An empty endpoint selects AWS's regional endpoint; otherwise use an HTTP(S)
 * origin without path/query/userinfo. TLS verification stays enabled.
 * Empty credentials select the SDK provider chain (environment, profiles, roles).
 * Explicit credentials are useful for MinIO; never include them in source IDs.
 */
struct S3Config {
    std::string endpoint;
    std::string region = "us-east-1";
    std::string access_key;
    std::string secret_key;
    std::string session_token;
    bool path_style = false;
};

/** Shared S3 connection pool and SDK lifetime. Copying shares the client.
 * Concurrent requests are supported. Sources retain the client, so this facade
 * may be destroyed before its sources. Configure once before publishing it.
 * Requests have bounded timeouts, no redirects and no automatic retries.
 * The SDK is initialized once per process by this module; do not initialize or
 * shut it down separately. All requests must finish before process teardown.
 */
class S3Client {
public:
    explicit S3Client(const S3Config& config);
private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
    friend class S3ByteSource;
};

/** Read-only S3 object, opened with HEAD and borrowed through ByteSource.
 * A non-null VersionId pins all GETs; otherwise every GET uses If-Match with the
 * captured ETag. An ETag is an opaque server validator, not a content hash.
 * Reads up to 4 KiB use an aligned, at-most-16-page LRU bound to this opened
 * source (64 KiB resident payload plus one incoming page). Larger reads bypass
 * it. No cross-source/request cache or whole-object allocation is implied.
 * Object keys are literal (not URL-decoded); the SDK performs URL encoding.
 * Concurrent small reads serialize through the per-source cache mutex, including
 * misses; direct large reads retain concurrent independent buffers. Copies share
 * this same opened-source cache. HEAD still runs on every new source opening.
 * Cached bytes retain the captured version; replacement becomes observable on a
 * remote miss or new opening. Once a change is observed, further nonempty reads
 * on this source fail even if a page was cached.
 * Debug logs expose anonymous operation lengths/offsets/durations and validation
 * outcome only; disabled logging performs no per-operation clock reads.
 */
class S3ByteSource final : public ByteSource {
public:
    S3ByteSource(const S3Client& client, std::string bucket, std::string key);
    [[nodiscard]] const SourceInfo& info() const noexcept override { return info_; }
    std::size_t read_at(std::uint64_t offset, std::span<std::byte> destination) const override;
private:
    struct SmallReads;
    std::shared_ptr<SmallReads> small_reads_;
    /** Fetch exactly the requested (already EOF-clipped) bytes using conditional
     * ranges. No cache access/locking; callers decide when to admit complete pages. */
    std::size_t read_remote(std::uint64_t offset, std::span<std::byte> destination) const;
    std::shared_ptr<S3Client::Impl> client_;
    std::string bucket_;
    std::string key_;
    std::string etag_;
    std::string version_id_;
    SourceInfo info_;
    std::uint64_t trace_id_ = 0; // Anonymous process-local diagnostic correlation, debug only.
};

} // namespace sipi
