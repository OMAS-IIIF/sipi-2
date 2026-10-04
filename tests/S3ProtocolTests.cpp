#include "storage/S3ByteSource.h"
#include <array>
#include <iostream>

/** Exercises hostile/incomplete HTTP responses from the local Python fixture.
 * Kept separate from MinIO tests because a conforming server cannot generate them. */
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Expected local fixture endpoint");
        sipi::S3Config config;
        config.endpoint = argv[1]; config.path_style = true;
        config.access_key = "test"; config.secret_key = "test-secret";
        sipi::S3Client client(config);
        std::array<std::byte, 2> bytes{};
        sipi::S3ByteSource valid(client, "bucket", "valid");
        if (valid.read_at(1, bytes) != 2 || bytes[0] != std::byte{'b'} || bytes[1] != std::byte{'c'})
            throw std::runtime_error("Valid range failed");
        using Code = sipi::ByteSourceErrorCode;
        for (const auto& [key, expected] : {
            std::pair{"ignored-range", Code::io}, {"wrong-range", Code::io},
            {"truncated", Code::io}, {"wrong-etag", Code::changed},
            {"precondition", Code::changed}, {"deleted", Code::changed},
            {"denied", Code::io}, {"no-etag", Code::io}, {"missing", Code::not_found}}) {
            bool failed = false;
            try {
                const sipi::S3ByteSource source(client, "bucket", key);
                source.read_at(1, bytes);
            } catch (const sipi::ByteSourceError& error) {
                if (error.code() != expected) throw std::runtime_error(std::string("Wrong error: ") + key);
                failed = true;
            }
            if (!failed) throw std::runtime_error(std::string("Accepted bad response: ") + key);
        }
        std::cout << "S3 malformed response checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
