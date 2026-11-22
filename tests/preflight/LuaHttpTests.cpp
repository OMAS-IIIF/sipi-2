#include "preflight/LuaPreflight.h"

#include <future>
#include <iostream>
#include <string_view>
#include <vector>

/** Python owns the local HTTP/TLS fixtures; this driver exercises only the public
 * evaluator, including startup validation and independent concurrent Lua states.
 *
 * Usage: lua-http-tests SCRIPT COUNT [sequential]
 * By default COUNT evaluations run concurrently. With "sequential" they run one
 * after another on one policy object, which lets the fixture observe whether
 * later evaluations reuse connections opened by earlier ones. */
int main(int argc, char** argv) {
    if (argc != 3 && argc != 4) return 2;
    const bool sequential = argc == 4 && std::string_view(argv[3]) == "sequential";
    if (argc == 4 && !sequential) return 2;
    try {
        const sipi::preflight::LuaPreflight policy(argv[1]);
        policy.validate();
        const auto evaluate = [&] {
            return policy.evaluate({{{}, "example"}, "Bearer inbound-secret", "session=inbound-secret"});
        };
        const int count = std::stoi(argv[2]);
        if (sequential) {
            for (int i = 0; i < count; ++i)
                if (!std::holds_alternative<sipi::preflight::Allow>(evaluate())) return 1;
            return 0;
        }
        std::vector<std::future<sipi::preflight::Decision>> results;
        for (int i = 0; i < count; ++i) results.push_back(std::async(std::launch::async, evaluate));
        for (auto& result : results)
            if (!std::holds_alternative<sipi::preflight::Allow>(result.get())) return 1;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
