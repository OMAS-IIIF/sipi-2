#include "preflight/LuaPreflight.h"

#include <future>
#include <iostream>
#include <vector>

/** Python owns the local HTTP/TLS fixtures; this driver exercises only the public
 * evaluator, including startup validation and independent concurrent Lua states. */
int main(int argc, char** argv) {
    if (argc != 3) return 2;
    try {
        const sipi::preflight::LuaPreflight policy(argv[1]);
        policy.validate();
        std::vector<std::future<sipi::preflight::Decision>> results;
        const int count = std::stoi(argv[2]);
        for (int i = 0; i < count; ++i) results.push_back(std::async(std::launch::async, [&] {
            return policy.evaluate({{{}, "example"}, "Bearer inbound-secret", "session=inbound-secret"});
        }));
        for (auto& result : results)
            if (!std::holds_alternative<sipi::preflight::Allow>(result.get())) return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
