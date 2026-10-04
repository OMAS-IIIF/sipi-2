#include "S3ByteSource.h"

#include <aws/core/Aws.h>
#include <aws/core/auth/AWSCredentialsProvider.h>
#include <aws/core/auth/AWSCredentialsProviderChain.h>
#include <aws/core/client/DefaultRetryStrategy.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>

#include <algorithm>
#include <cstring>
#include <streambuf>

namespace sipi {
namespace {

/** Function-static initialization is thread safe. Registered before any clients
 * constructed through it; clients and their requests must not outlive teardown. */
struct SdkRuntime {
    Aws::SDKOptions options;
    SdkRuntime() { Aws::InitAPI(options); }
    ~SdkRuntime() { Aws::ShutdownAPI(options); }
};
void initialize_sdk() { static SdkRuntime runtime; }

void require(bool valid) {
    if (!valid) throw ByteSourceError(ByteSourceErrorCode::invalid_argument, "Invalid S3 source configuration");
}
bool clean(const std::string& value) { return value.find('\0') == std::string::npos; }

/** Length framing avoids collisions between arbitrary bucket/key components. */
std::string component(const std::string& value) { return std::to_string(value.size()) + ':' + value; }

/** Writes directly into the caller's bounded buffer, including on error replies.
 * A server ignoring Range cannot make the SDK allocate an entire image body.
 * No exception crosses the HTTP callback; excess bytes fail the stream write.
 */
class ResponseBuffer final : public std::streambuf {
public:
    explicit ResponseBuffer(std::span<std::byte> bytes) : bytes_(bytes) {}
    std::size_t written() const noexcept { return written_; }
protected:
    std::streamsize xsputn(const char* data, std::streamsize count) override {
        if (count <= 0) return 0;
        const auto size = std::min(static_cast<std::size_t>(count), bytes_.size() - written_);
        std::memcpy(bytes_.data() + written_, data, size);
        written_ += size;
        return static_cast<std::streamsize>(size);
    }
    int_type overflow(int_type ch) override {
        if (traits_type::eq_int_type(ch, traits_type::eof())) return traits_type::not_eof(ch);
        if (written_ == bytes_.size()) return traits_type::eof();
        bytes_[written_++] = static_cast<std::byte>(traits_type::to_char_type(ch));
        return ch;
    }
private:
    std::span<std::byte> bytes_;
    std::size_t written_ = 0;
};

[[noreturn]] void source_failure(Aws::Http::HttpResponseCode status, bool opened) {
    auto code = ByteSourceErrorCode::io;
    if (status == Aws::Http::HttpResponseCode::PRECONDITION_FAILED ||
        (opened && status == Aws::Http::HttpResponseCode::NOT_FOUND))
        code = ByteSourceErrorCode::changed;
    else if (status == Aws::Http::HttpResponseCode::NOT_FOUND)
        code = ByteSourceErrorCode::not_found;
    // SDK diagnostics can contain endpoints/keys: never propagate them publicly.
    throw ByteSourceError(code, "S3 source request failed");
}
} // namespace

struct S3Client::Impl {
    std::string identity;
    std::unique_ptr<Aws::S3::S3Client> sdk;
};

S3Client::S3Client(const S3Config& config) {
    require(!config.region.empty() && clean(config.region));
    require(clean(config.access_key) && clean(config.secret_key) && clean(config.session_token));
    require(config.access_key.empty() == config.secret_key.empty());
    require(config.session_token.empty() || !config.access_key.empty());
    if (!config.endpoint.empty()) {
        const auto prefix = config.endpoint.starts_with("https://") ? 8U :
                            config.endpoint.starts_with("http://") ? 7U : 0U;
        require(prefix != 0 && config.endpoint.size() > prefix && clean(config.endpoint));
        require(config.endpoint.find_first_of("/?#@ \t\r\n", prefix) == std::string::npos);
    }
    initialize_sdk();
    // Region is explicit: never probe EC2 metadata merely to infer it.
    Aws::Client::ClientConfigurationInitValues initial;
    initial.shouldDisableIMDS = true;
    Aws::Client::ClientConfiguration options(initial);
    options.region = config.region;
    options.endpointOverride = config.endpoint;
    options.connectTimeoutMs = 2000;
    options.httpRequestTimeoutMs = 10000;
    options.requestTimeoutMs = 10000;
    options.followRedirects = Aws::Client::FollowRedirectsPolicy::NEVER;
    options.retryStrategy = Aws::MakeShared<Aws::Client::DefaultRetryStrategy>("sipi-s3", 0);
    std::shared_ptr<Aws::Auth::AWSCredentialsProvider> credentials;
    if (config.access_key.empty())
        credentials = Aws::MakeShared<Aws::Auth::DefaultAWSCredentialsProviderChain>("sipi-s3");
    else
        credentials = Aws::MakeShared<Aws::Auth::SimpleAWSCredentialsProvider>(
            "sipi-s3", config.access_key, config.secret_key, config.session_token);
    impl_ = std::make_shared<Impl>();
    impl_->identity = component(config.endpoint.empty() ? "aws" : config.endpoint) + component(config.region);
    impl_->sdk = std::make_unique<Aws::S3::S3Client>(credentials, options,
        Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never, !config.path_style);
}

S3ByteSource::S3ByteSource(const S3Client& client, std::string bucket, std::string key)
    : client_(client.impl_), bucket_(std::move(bucket)), key_(std::move(key)) {
    require(client_ && !bucket_.empty() && !key_.empty() && clean(bucket_) && clean(key_));
    // This adapter addresses ordinary buckets, not access-point ARNs or URLs.
    require(bucket_.find_first_of("/:?#@ \t\r\n") == std::string::npos);
    Aws::S3::Model::HeadObjectRequest request;
    request.SetBucket(bucket_);
    request.SetKey(key_);
    const auto outcome = client_->sdk->HeadObject(request);
    if (!outcome.IsSuccess()) source_failure(outcome.GetError().GetResponseCode(), false);
    const auto& result = outcome.GetResult();
    if (result.GetContentLength() < 0 || result.GetETag().empty())
        throw ByteSourceError(ByteSourceErrorCode::io, "Invalid S3 source metadata");
    info_.size = static_cast<std::uint64_t>(result.GetContentLength());
    etag_ = result.GetETag();
    version_id_ = result.GetVersionId();
    if (version_id_ == "null") version_id_.clear(); // null versions can be replaced
    info_.identity = "s3:" + client_->identity + component(bucket_) + component(key_);
    info_.version = version_id_.empty() ? "etag:" + component(etag_) + std::to_string(info_.size)
                                       : "version:" + component(version_id_);
}

std::size_t S3ByteSource::read_at(std::uint64_t offset, std::span<std::byte> destination) const {
    if (offset >= info_.size || destination.empty()) return 0;
    const auto wanted = static_cast<std::size_t>(std::min<std::uint64_t>(destination.size(), info_.size - offset));
    std::size_t completed = 0;
    constexpr std::size_t max_range = 1024 * 1024;
    while (completed < wanted) {
        const auto count = std::min(max_range, wanted - completed);
        const auto start = offset + completed;
        const auto end = start + count - 1; // bounded by captured size
        const auto range = std::to_string(start) + '-' + std::to_string(end);
        ResponseBuffer buffer(destination.subspan(completed, count));
        Aws::S3::Model::GetObjectRequest request;
        request.SetBucket(bucket_);
        request.SetKey(key_);
        request.SetRange("bytes=" + range);
        request.SetIfMatch(etag_);
        if (!version_id_.empty()) request.SetVersionId(version_id_);
        request.SetResponseStreamFactory([&buffer] { return Aws::New<Aws::IOStream>("sipi-s3", &buffer); });
        const auto outcome = client_->sdk->GetObject(request);
        if (!outcome.IsSuccess()) source_failure(outcome.GetError().GetResponseCode(), true);
        const auto& result = outcome.GetResult();
        if (result.GetETag() != etag_ || (!version_id_.empty() && result.GetVersionId() != version_id_))
            throw ByteSourceError(ByteSourceErrorCode::changed, "S3 source version changed");
        if (result.GetContentLength() != static_cast<long long>(count) || buffer.written() != count ||
            result.GetContentRange() != "bytes " + range + '/' + std::to_string(info_.size))
            throw ByteSourceError(ByteSourceErrorCode::io, "Invalid S3 range response");
        completed += count;
    }
    return completed;
}
} // namespace sipi
