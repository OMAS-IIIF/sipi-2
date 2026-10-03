#include "preflight/LuaPreflight.h"

#include <fstream>
#include <future>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
using namespace sipi::preflight;
int checks = 0;
int failures = 0;
std::filesystem::path directory;

void check(bool condition, std::string_view description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << description << '\n';
    }
}

/** Each fixture is a real text file, exercising the public loading boundary. */
LuaPreflight script(std::string_view code) {
    const auto path = directory / "policy.lua";
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << code;
    output.close();
    if (!output) throw std::runtime_error("cannot write test script");
    return LuaPreflight(path);
}

void rejects(std::string_view code, ErrorCode expected, std::string_view label) {
    try {
        (void)script(code).evaluate({{{}, "example"}, std::nullopt, std::nullopt});
        check(false, label);
    } catch (const PreflightError& error) {
        check(error.code() == expected && std::string_view(error.what()).size() > 0, label);
    }
}

/** Check transport without conflating decoded identifiers and local paths. */
void request_and_decision() {
    const auto policy = script(R"lua(
        function preflight(request)
            assert(#request.prefix == 2)
            assert(request.prefix[1] == "project/part")
            assert(request.prefix[2] == "space here")
            assert(request.identifier == "id%2Fvalue/é")
            assert(request.headers.authorization == "Bearer example")
            assert(request.headers.cookie == "session=abc")
            return { decision = "allow", source = "/images/é example.tif" }
        end
    )lua");
    const auto result = policy.evaluate({{{"project/part", "space here"}, "id%2Fvalue/é"},
                                         "Bearer example", "session=abc"});
    const auto* allow = std::get_if<Allow>(&result);
    check(allow && allow->source == std::filesystem::path("/images/é example.tif"),
          "owning source survives Lua-state destruction, without filesystem lookup");

    const auto no_headers = script(R"lua(
        function preflight(request)
            assert(#request.prefix == 0)
            assert(request.headers.authorization == nil)
            assert(request.headers.cookie == nil)
            return {decision = "allow"}
        end
    )lua").evaluate({{{}, "example"}, std::nullopt, std::nullopt});
    check(std::holds_alternative<Allow>(no_headers) && !std::get<Allow>(no_headers).source,
          "allow without override and missing headers");

    check(std::holds_alternative<Deny>(script(R"lua(
        function preflight(request)
            assert(request.headers.authorization == "")
            assert(request.headers.cookie == "")
            return {decision = "deny"}
        end
    )lua").evaluate({{{}, "example"}, "", ""})), "empty headers differ from missing headers");

    const auto binary = script(R"lua(
        function preflight(request)
            assert(#request.headers.cookie == 3)
            assert(string.byte(request.headers.cookie, 2) == 0)
            return {decision = "allow", source = "relative.tif"}
        end
    )lua").evaluate({{{}, "example"}, std::nullopt, std::string("a\0b", 3)});
    check(std::get<Allow>(binary).source == std::filesystem::path("relative.tif"),
          "length-aware input strings and unmodified relative source");
}

void invalid_scripts_and_results() {
    rejects("function preflight(", ErrorCode::script_load, "syntax error");
    rejects("error('initialization failed')", ErrorCode::script_execution, "top-level failure");
    rejects("", ErrorCode::contract, "missing function");
    rejects("preflight = 42", ErrorCode::contract, "non-function entry point");
    rejects("function preflight() error('request failed') end", ErrorCode::script_execution,
            "function failure");
    rejects("function preflight() error({secret = 'value'}) end", ErrorCode::script_execution,
            "non-string error object");
    const std::vector<std::string> invalid_returns = {
        "", "nil", "true", "42", "'allow'", "{}",
        "{decision = 'allow'}, nil", "{decision = 'allow'}, {decision = 'deny'}",
        "{decision = true}", "{decision = 1}", "{decision = 'ALLOW'}",
        "{decision = 'restrict'}", "{decision = 'allow' .. string.char(0)}",
        "{decision = 'allow', source = ''}", "{decision = 'allow', source = 123}",
        "{decision = 'allow', source = false}", "{decision = 'allow', source = {}}",
        "{decision = 'allow', source = 'a' .. string.char(0) .. 'b'}",
        "{decision = 'deny', source = '/image.tif'}",
        "{decision = 'allow', sorce = '/image.tif'}",
        "{decision = 'allow', restrictions = {width = 1000}}",
        "{decision = 'allow', [1] = 'value'}",
        "setmetatable({}, {__index = function() return 'allow' end})"
    };
    for (const auto& value : invalid_returns) {
        rejects("function preflight() return " + value + " end", ErrorCode::contract, value);
    }

    try {
        LuaPreflight missing(directory / "missing" / "policy.lua");
        check(false, "unreadable script");
    } catch (const PreflightError& error) {
        check(error.code() == ErrorCode::script_file, "unreadable script");
    }
}

/** Reusing one policy must neither leak globals nor retain prior decisions. */
void isolation_and_snapshot() {
    const auto policy = script(R"lua(
        calls = (calls or 0) + 1
        assert(calls == 1)
        function preflight(request)
            assert(previous == nil)
            previous = request.identifier
            if request.identifier == 'error' then error('expected failure') end
            if request.identifier == 'invalid' then return {decision = 'allow', typo = true} end
            if request.identifier == 'deny' then return {decision = 'deny'} end
            return {decision = 'allow', source = request.identifier}
        end
    )lua");
    // Replacing the file does not alter the already loaded script snapshot.
    (void)script("function preflight() return {decision = 'deny'} end");
    for (int i = 0; i < 20; ++i) {
        for (const auto& identifier : {"error", "invalid"}) {
            try {
                (void)policy.evaluate({{{}, identifier}, std::nullopt, std::nullopt});
                check(false, "expected failure on reused evaluator");
            } catch (const PreflightError& error) {
                check(error.code() == (std::string_view(identifier) == "error"
                          ? ErrorCode::script_execution : ErrorCode::contract), "failure classification");
            }
        }
        const auto result = policy.evaluate({{{}, "unique.tif"}, std::nullopt, std::nullopt});
        check(std::holds_alternative<Allow>(result) &&
              std::get<Allow>(result).source == std::filesystem::path("unique.tif"),
              "success after runtime/contract failure; isolated globals and script snapshot");
        check(std::holds_alternative<Deny>(policy.evaluate({{{}, "deny"}, {}, {}})),
              "deny never reuses a previous allow");
    }

    std::vector<std::future<Decision>> concurrent;
    for (int i = 0; i < 8; ++i) {
        concurrent.push_back(std::async(std::launch::async, [&policy, i] {
            return policy.evaluate({{{}, std::to_string(i) + ".tif"}, {}, {}});
        }));
    }
    for (std::size_t i = 0; i < concurrent.size(); ++i) {
        const auto result = concurrent[i].get();
        check(std::get<Allow>(result).source == std::filesystem::path(std::to_string(i) + ".tif"),
              "concurrent calls have independent states");
    }
}

/** Startup validation initializes the chunk but never calls preflight(request). */
void startup_validation() {
    const auto valid = script("function preflight(request) error('request only') end");
    valid.validate();
    valid.validate();
    check(true, "startup validation does not invoke the request function");
    for (const auto& [code, expected] : std::vector<std::pair<std::string, ErrorCode>>{
             {"function preflight(", ErrorCode::script_load},
             {"error('init failed')", ErrorCode::script_execution},
             {"", ErrorCode::contract}, {"preflight = 42", ErrorCode::contract}}) {
        try {
            script(code).validate();
            check(false, "startup rejects invalid script");
        } catch (const PreflightError& error) {
            check(error.code() == expected, "startup error classification");
        }
    }
}

/** Budgets cover startup and calls, including attempts to catch quota errors. */
void resource_limits() {
    rejects("function preflight() while true do end end", ErrorCode::instruction_limit, "infinite loop");
    rejects("function preflight() while true do pcall(function() while true do end end) end end",
            ErrorCode::instruction_limit, "caught quota error cannot reset budget");
    rejects("function preflight() return {decision='allow', source=string.rep('x', 32*1024*1024)} end",
            ErrorCode::memory, "Lua allocation budget");
    try {
        script("while true do end").validate();
        check(false, "startup instruction limit");
    } catch (const PreflightError& error) {
        check(error.code() == ErrorCode::instruction_limit, "startup instruction limit");
    }
    check(std::holds_alternative<Deny>(script("function preflight() return {decision='deny'} end")
          .evaluate({{{}, "example"}, {}, {}})), "normal call after exhausted budgets");
}

void library_contract() {
    check(std::holds_alternative<Allow>(script(R"lua(
        assert(type(table.concat) == 'function')
        assert(type(string.sub) == 'function')
        assert(type(math.floor) == 'function')
        assert(type(utf8.len) == 'function')
        assert(io == nil and os == nil and package == nil and debug == nil)
        assert(load == nil and loadfile == nil and dofile == nil)
        assert(print == nil and warn == nil)
        function preflight() return {decision = 'allow'} end
    )lua").evaluate({{{}, "example"}, {}, {}})), "documented standard library subset");
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    directory = argv[1];
    std::filesystem::create_directories(directory);
    try {
        request_and_decision();
        invalid_scripts_and_results();
        isolation_and_snapshot();
        library_contract();
        startup_validation();
        resource_limits();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected error: " << error.what() << '\n';
        return 1;
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
