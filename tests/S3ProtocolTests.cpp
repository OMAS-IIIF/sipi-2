#include "storage/S3ByteSource.h"
#include <array>
#include <iostream>
#include <sstream>
#include <future>
#include <vector>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/ostream_sink.h>

/** Exercises hostile/incomplete HTTP responses from the local Python fixture.
 * Kept separate from MinIO tests because a conforming server cannot generate them. */
int main(int argc, char** argv) {
    try {
        if (argc != 2) throw std::runtime_error("Expected local fixture endpoint");
        sipi::S3Config config;
        config.endpoint = argv[1]; config.path_style = true;
        config.access_key = "test"; config.secret_key = "test-secret";
        std::ostringstream diagnostics;
        auto sink = std::make_shared<spdlog::sinks::ostream_sink_mt>(diagnostics);
        auto logger = std::make_shared<spdlog::logger>("s3-test",sink);
        logger->set_pattern("%v");
        logger->set_level(spdlog::level::debug);
        spdlog::set_default_logger(logger);
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
        const auto log = diagnostics.str();
        for (const auto* required : {"S3 io op=HEAD source=1 object_bytes=4", "S3 io op=GET source=1 object_bytes=4 offset=0 requested=4 received=4",
                                    "accepted=1", "accepted=0"})
            if (log.find(required)==std::string::npos) throw std::runtime_error("Missing I/O diagnostic");
        for (const auto* secret : {"bucket", "test-secret", "wrong-etag", "original", "127.0.0.1"})
            if (log.find(secret)!=std::string::npos) throw std::runtime_error("Private source data in diagnostic");
        logger->set_level(spdlog::level::warn);
        const sipi::S3ByteSource quiet(client,"bucket","valid");
        quiet.read_at(1,bytes);
        if (diagnostics.str()!=log) throw std::runtime_error("I/O diagnostics were not opt-in");
        const auto verify = [](const sipi::S3ByteSource& source, std::uint64_t offset, std::size_t count) {
            std::vector<std::byte> output(count);
            const auto wanted = offset >= source.info().size ? 0 : std::min<std::uint64_t>(count,source.info().size-offset);
            if (source.read_at(offset,output)!=wanted) throw std::runtime_error("Buffered read count mismatch");
            for (std::size_t j=0;j<wanted;++j)
                if (output[j]!=static_cast<std::byte>((offset+j)%251)) throw std::runtime_error("Buffered bytes mismatch");
        };
        const sipi::S3ByteSource reuse(client,"bucket","cache-reuse");
        verify(reuse,3,2); verify(reuse,100,80);
        const auto shared = reuse; // Copies retain the same opened snapshot/cache.
        verify(shared,8,4);
        verify(reuse,1,5000); // Large reads stay exact, direct, and do not fill pages.
        verify(reuse,4096,2); // The previous direct read did not populate this page.
        const sipi::S3ByteSource boundary(client,"bucket","cache-boundary");
        verify(boundary,4094,4); verify(boundary,4095,4096);
        verify(boundary,81930,20); verify(boundary,81937,2); verify(boundary,UINT64_MAX,2);
        const sipi::S3ByteSource eviction(client,"bucket","cache-eviction");
        for (unsigned page=0;page<16;++page) verify(eviction,page*4096,1);
        verify(eviction,0,1); // Promote page zero before admitting the seventeenth.
        verify(eviction,16*4096,1); verify(eviction,0,1); verify(eviction,4096,1);
        const sipi::S3ByteSource parallel(client,"bucket","cache-parallel");
        std::vector<std::future<void>> jobs;
        for (unsigned i=0;i<8;++i) jobs.push_back(std::async(std::launch::async,[&,i] { verify(parallel,i*10,20); }));
        for (auto& job:jobs) job.get();
        bool failed=false;
        for (const auto* key : {"cache-failure","cache-partial"}) {
            const sipi::S3ByteSource failure(client,"bucket",key);
            failed=false;
            try { verify(failure,5,2); } catch (const sipi::ByteSourceError& error) { failed=error.code()==Code::io; }
            if (!failed) throw std::runtime_error("Expected first fill failure");
            verify(failure,5,2); verify(failure,5,2); // Failed/partial pages are not admitted.
        }
        const sipi::S3ByteSource changed(client,"bucket","cache-version");
        verify(changed,5,2);
        for (auto offset : {4096U,5U}) {
            failed=false;
            try { verify(changed,offset,2); } catch (const sipi::ByteSourceError& error) { failed=error.code()==Code::changed; }
            if (!failed) throw std::runtime_error("Changed source served a cached page");
        }
        const sipi::S3ByteSource large_changed(client,"bucket","cache-version-large");
        verify(large_changed,5,2);
        for (auto offset : {4096U,5U}) {
            failed=false;
            try { verify(large_changed,offset,offset==4096 ? 5000 : 2); }
            catch (const sipi::ByteSourceError& error) { failed=error.code()==Code::changed; }
            if (!failed) throw std::runtime_error("Large-read change did not poison cached page");
        }
        std::cout << "S3 malformed response and opt-in diagnostic checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
