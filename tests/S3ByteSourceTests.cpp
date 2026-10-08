#include "storage/S3ByteSource.h"
#include "storage/LocalFileSource.h"
#include "imgformats/TiffReader.h"

#include <aws/s3/S3Client.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/utils/memory/stl/AWSStringStream.h>
#include <aws/s3/model/CreateBucketRequest.h>
#include <aws/s3/model/DeleteBucketRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/ListObjectVersionsRequest.h>
#include <aws/s3/model/PutBucketVersioningRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <chrono>
#include <future>
#include <iostream>
#include <limits>

namespace {
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void expect(sipi::ByteSourceErrorCode code, F function) {
    try { function(); } catch (const sipi::ByteSourceError& e) {
        check(e.code() == code, "Wrong storage error code"); return;
    }
    throw std::runtime_error("Expected storage error");
}

/** Own only uniquely named disposable test buckets; never alter the manual bucket.
 * Test provisioning uses SDK operations directly, outside the read-only adapter. */
class Bucket {
public:
    Aws::S3::S3Client& client;
    std::string name;
    Bucket(Aws::S3::S3Client& client, std::string name) : client(client), name(std::move(name)) {
        Aws::S3::Model::CreateBucketRequest request; request.SetBucket(this->name);
        check(client.CreateBucket(request).IsSuccess(), "Create test bucket failed");
    }
    ~Bucket() {
        // A handful of objects/versions only; fail visibly if cleanup cannot finish.
        Aws::S3::Model::ListObjectVersionsRequest list; list.SetBucket(name);
        const auto entries = client.ListObjectVersions(list);
        bool ok = entries.IsSuccess();
        if (ok) {
            const auto remove = [&](const auto& entry) {
                Aws::S3::Model::DeleteObjectRequest request;
                request.SetBucket(name); request.SetKey(entry.GetKey()); request.SetVersionId(entry.GetVersionId());
                ok = client.DeleteObject(request).IsSuccess() && ok;
            };
            for (const auto& entry : entries.GetResult().GetVersions()) remove(entry);
            for (const auto& entry : entries.GetResult().GetDeleteMarkers()) remove(entry);
        }
        Aws::S3::Model::DeleteBucketRequest request; request.SetBucket(name);
        if (!client.DeleteBucket(request).IsSuccess() || !ok)
            std::cerr << "Test cleanup failed for " << name << '\n';
    }
    void put(const std::string& key, const std::string& value) {
        Aws::S3::Model::PutObjectRequest request; request.SetBucket(name); request.SetKey(key);
        auto body = Aws::MakeShared<Aws::StringStream>("sipi-test");
        body->write(value.data(), static_cast<std::streamsize>(value.size()));
        request.SetBody(body); request.SetContentLength(static_cast<long long>(value.size()));
        check(client.PutObject(request).IsSuccess(), "Upload fixture failed");
    }
    void versioning(Aws::S3::Model::BucketVersioningStatus status) {
        Aws::S3::Model::PutBucketVersioningRequest request; request.SetBucket(name);
        Aws::S3::Model::VersioningConfiguration config; config.SetStatus(status);
        request.SetVersioningConfiguration(config);
        check(client.PutBucketVersioning(request).IsSuccess(), "Set versioning failed");
    }
    void erase(const std::string& key) {
        Aws::S3::Model::DeleteObjectRequest request; request.SetBucket(name); request.SetKey(key);
        check(client.DeleteObject(request).IsSuccess(), "Delete fixture failed");
    }
};
std::string read(const sipi::ByteSource& source, std::uint64_t offset, std::size_t length) {
    std::string data(length, '\0');
    data.resize(source.read_at(offset, std::as_writable_bytes(std::span{data})));
    return data;
}
} // namespace

int main(int argc, char** argv) {
    try {
        check(argc == 2, "Expected TIFF fixture path");
        sipi::S3Config config;
        config.endpoint = "http://127.0.0.1:9000";
        config.access_key = "sipi-test"; config.secret_key = "sipi-local-test-password"; config.path_style = true;
        const sipi::S3Client client(config); // initializes SDK before fixture client
        Aws::Client::ClientConfigurationInitValues initial;
        initial.shouldDisableIMDS = true;
        Aws::Client::ClientConfiguration options(initial);
        options.endpointOverride = config.endpoint; options.region = config.region;
        options.connectTimeoutMs = 1000; options.httpRequestTimeoutMs = 5000;
        Aws::S3::S3Client fixtures(Aws::Auth::AWSCredentials(config.access_key, config.secret_key), options,
            Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never, false);
        const auto suffix = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        Bucket plain(fixtures, "sipi-test-plain-" + suffix);
        Bucket versioned(fixtures, "sipi-test-versioned-" + suffix);
        versioned.versioning(Aws::S3::Model::BucketVersioningStatus::Enabled);
        std::string data(2 * 1024 * 1024 + 57, '\0');
        for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i % 251);
        const std::string key = "folder/a b+%?#.bin";
        plain.put(key, data);
        const sipi::S3ByteSource source(client, plain.name, key);
        check(source.info().size == data.size(), "HEAD size");
        check(read(source, 0, data.size()) == data, "Multi-range read differs");
        check(read(source, data.size() - 5, 50) == data.substr(data.size() - 5), "EOF clipping");
        check(read(source, data.size(), 5).empty(), "EOF");
        check(read(source, std::numeric_limits<std::uint64_t>::max(), 5).empty(), "Offset overflow");
        check(read(source, 1, 0).empty(), "Empty buffer");
        std::vector<std::future<void>> reads;
        for (int i = 0; i < 6; ++i) reads.push_back(std::async(std::launch::async, [&, i] {
            check(read(source, i * 137, 4097) == data.substr(i * 137, 4097), "Concurrent read");
        }));
        for (auto& task : reads) task.get();
        plain.put("empty", "");
        check(read(sipi::S3ByteSource(client, plain.name, "empty"), 0, 5).empty(), "Empty object");
        expect(sipi::ByteSourceErrorCode::not_found, [&] { sipi::S3ByteSource missing(client, plain.name, "absent"); });
        plain.put(key, "replacement");
        expect(sipi::ByteSourceErrorCode::changed, [&] { read(source, 0, 16); });
        expect(sipi::ByteSourceErrorCode::changed, [&] { read(source, data.size()-5, 5); }); // Warm page poisoned too.
        const sipi::S3ByteSource replaced(client, plain.name, key);
        check(read(replaced, 0, 100) == "replacement", "New open sees replacement");
        check(source.info().version != replaced.info().version, "Version fingerprints differ");
        const sipi::S3ByteSource unopened_page(client, plain.name, key);
        plain.erase(key);
        check(read(replaced, 0, 4)=="repl", "Cached bytes preserve opened snapshot after deletion");
        expect(sipi::ByteSourceErrorCode::changed, [&] { read(unopened_page, 0, 4); });
        expect(sipi::ByteSourceErrorCode::not_found, [&] { sipi::S3ByteSource missing(client,plain.name,key); });

        versioned.put("image", "original");
        const sipi::S3ByteSource pinned(client, versioned.name, "image");
        versioned.put("image", "replacement");
        check(read(pinned, 0, 100) == "original", "Pinned version survives replacement");
        versioned.erase("image");
        check(read(pinned, 0, 100) == "original", "Pinned version survives delete marker");
        versioned.versioning(Aws::S3::Model::BucketVersioningStatus::Suspended);
        versioned.put("null-version", "before");
        const sipi::S3ByteSource null_version(client, versioned.name, "null-version");
        versioned.put("null-version", "after");
        expect(sipi::ByteSourceErrorCode::changed, [&] { read(null_version, 0, 3); });

        const sipi::LocalFileSource local(argv[1]);
        plain.put("image.tif", read(local, 0, static_cast<std::size_t>(local.info().size)));
        const sipi::S3ByteSource remote(client, plain.name, "image.tif");
        const auto lm = sipi::read_tiff_metadata(local);
        const auto rm = sipi::read_tiff_metadata(remote);
        check(lm.image.display_width() == rm.image.display_width() && lm.image.display_height() == rm.image.display_height(), "TIFF metadata differs");
        for (const sipi::PixelRegion region : {sipi::PixelRegion{0, 0, 320, 200}, {50, 30, 100, 80}, {319, 199, 1, 1}}) {
            const auto a = sipi::read_tiff_region(local, region);
            const auto b = sipi::read_tiff_region(remote, region);
            check(a.width == b.width && a.height == b.height && a.pixels == b.pixels && a.icc_profile == b.icc_profile, "TIFF pixels differ");
        }
        plain.put("image.tif", "not the original TIFF");
        expect(sipi::ByteSourceErrorCode::changed, [&] { (void)sipi::read_tiff_region(remote, {0, 0, 1, 1}); });
        auto bad = config; bad.secret_key = "incorrect-test-secret";
        expect(sipi::ByteSourceErrorCode::io, [&] { sipi::S3ByteSource denied(sipi::S3Client(bad), plain.name, "image.tif"); });
        auto invalid = config; invalid.endpoint = "http://user:secret@localhost:9000";
        expect(sipi::ByteSourceErrorCode::invalid_argument, [&] { sipi::S3Client rejected(invalid); });
        std::cout << "S3 ranges, versions, concurrency and local/remote TIFF equivalence passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
