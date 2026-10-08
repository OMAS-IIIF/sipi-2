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

void resolution_contract() {
    const auto decision = script("function preflight() return {decision='allow',resolution={max_width=1000,max_height=800}} end")
        .evaluate({{{},"image"},{},{}});
    const auto& limit = *std::get<Allow>(decision).resolution;
    check(limit.max_width==1000 && limit.max_height==800,"owning resolution policy");
    for (const std::string edit : {"r.max_width=nil","r.max_height=nil","r.max_width=0",
        "r.max_height=-1","r.max_width=4294967296","r.max_width='1000'","r.max_width=0/0",
        "r.max_height=math.huge","r.max_width=1.5","r.extra=1","r=false","r=setmetatable(r,{})"})
        rejects("function preflight() local r={max_width=1000,max_height=800}; "+edit+
            "; return {decision='allow',resolution=r} end",ErrorCode::contract,"invalid resolution");
    rejects("function preflight() return {decision='deny',resolution={max_width=1,max_height=1}} end",
        ErrorCode::contract,"deny rejects resolution");
    const auto both = script("function preflight() return {decision='allow',source='image.tif',resolution={max_width=1,max_height=2},watermark={source='mask.png',width_fraction=1,opacity=1,color={255,0,0}}} end")
        .evaluate({{{},"image"},{},{}});
    check(std::get<Allow>(both).resolution->max_height==2 && std::get<Allow>(both).watermark->color[0]==255,
        "resolution combines with watermark and source override");
}

void watermark_contract() {
    const std::string valid = "{source='mask.png',width_fraction=0.5,opacity=0.25,color={255,128,0}}";
    const auto result=script("function preflight() return {decision='allow',watermark="+valid+"} end")
        .evaluate({{{},"example"},{},{}});
    const auto& allow=std::get<Allow>(result);
    check(!allow.source && allow.watermark.has_value(),"watermark without main source override");
    const auto& mark=*allow.watermark;
    check(std::get<std::filesystem::path>(mark.source)=="mask.png" && mark.width_fraction==0.5 &&
          mark.opacity==0.25 && mark.color==std::array<std::uint8_t,3>{255,128,0},"owning watermark fields");
    const auto remote=script(R"lua(function preflight() return {decision='allow',source='image.tif',
        watermark={source={type='s3',key='marks/logo.png'},width_fraction=1,opacity=0,color={0,0,0}}} end)lua")
        .evaluate({{{},"example"},{},{}});
    check(std::get<sipi::S3Key>(std::get<Allow>(remote).watermark->source).key=="marks/logo.png",
          "S3 watermark and independent local image source");
    for (const std::string edit : {
        "m.source=nil", "m.source=''", "m.source='x'..string.char(0)",
        "m.source={type='s3',key=''}", "m.source={type='s3',key='a',bucket='other'}",
        "m.source=setmetatable({type='s3',key='a'}, {})", "m.width_fraction=nil",
        "m.width_fraction='0.5'", "m.width_fraction=0", "m.width_fraction=1.01",
        "m.width_fraction=0/0", "m.width_fraction=math.huge", "m.opacity=nil",
        "m.opacity=-0.1", "m.opacity=1.1", "m.opacity='0.5'", "m.opacity=0/0",
        "m.opacity=math.huge", "m.color=nil", "m.color={255,0}", "m.color={255,0,0,0}",
        "m.color={255,-1,0}", "m.color={256,0,0}", "m.color={0.5,0,0}",
        "m.color={'255',0,0}", "m.color={[1]=1,[3]=3}", "m.color.extra=1",
        "m.color=setmetatable({0,0,0},{})", "m.extra=true", "m=setmetatable(m,{})", "m=false"}) {
        rejects("function preflight() local m="+valid+"; "+edit+"; return {decision='allow',watermark=m} end",
            ErrorCode::contract,"invalid watermark contract");
    }
    rejects("function preflight() return {decision='deny',watermark="+valid+"} end",
        ErrorCode::contract,"deny rejects watermark");
    const auto policy=script("function preflight() return {decision='allow',watermark="+valid+"} end");
    std::vector<std::future<Decision>> jobs;
    for (unsigned i=0;i<4;++i) jobs.push_back(std::async(std::launch::async,[&]{return policy.evaluate({{{},"id"},{},{}});}));
    for (auto& job:jobs) check(std::get<Allow>(job.get()).watermark->color==mark.color,"isolated watermark evaluation");
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
            assert(request.cookies.session == "abc")
            assert(request.kind == "info" and request.method == "HEAD")
            assert(request.query.token[1] == "first" and request.query.token[2] == "second")
            assert(#request.query.token == 2 and request.query.empty[1] == "")
            assert(request.query.missing == nil)
            request.query.token[1] = "changed"
            return { decision = "allow", source = "/images/é example.tif" }
        end
    )lua");
    const auto result = policy.evaluate({{{"project/part", "space here"}, "id%2Fvalue/é"},
                                         "Bearer example", "session=abc", {{"session", "abc"}},
                                         {{"token", {"first", "second"}}, {"empty", {""}}},
                                         RequestKind::info, RequestMethod::head});
    const auto* allow = std::get_if<Allow>(&result);
    check(allow && allow->source == std::optional<sipi::SourceOverride>{std::filesystem::path("/images/é example.tif")},
          "owning source survives Lua-state destruction, without filesystem lookup");

    const auto no_headers = script(R"lua(
        function preflight(request)
            assert(#request.prefix == 0)
            assert(next(request.query) == nil)
            assert(request.kind == "image" and request.method == "GET")
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
    check(std::get<Allow>(binary).source == std::optional<sipi::SourceOverride>{std::filesystem::path("relative.tif")},
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
        "{decision='allow', source={type='s3', key=''}}",
        "{decision='allow', source={type='s3', key=42}}",
        "{decision='allow', source={type='s3', key='a', bucket='other'}}",
        "{decision='allow', source={type='s3', key='a' .. string.char(0)}}",
        "{decision='allow', source={type='local', key='a'}}",
        "{decision='allow', source=setmetatable({type='s3',key='a'}, {})}",
        "{decision='deny', source={type='s3',key='a'}}",
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
              std::get<Allow>(result).source == std::optional<sipi::SourceOverride>{std::filesystem::path("unique.tif")},
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
        check(std::get<Allow>(result).source == std::optional<sipi::SourceOverride>{std::filesystem::path(std::to_string(i) + ".tif")},
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

/** Exercise JSON through the public evaluator, including startup and protected errors. */
void json_contract() {
    const auto policy = script(R"lua(
        local j = sipi.json
        assert(j.encode(j.decode('null')) == 'null')
        assert(j.encode(j.decode('[]')) == '[]')
        assert(j.encode(j.decode('{}')) == '{}')
        function preflight()
            local value = j.decode('{"items":[null,false,{"name":"é\\u0000x","n":42}],"ok":true}')
            assert(value.items[1] == j.null and value.items[2] == false)
            assert(value.items[3].name == 'é' .. string.char(0) .. 'x')
            assert(value.ok and value.items[3].n == 42)
            local copy = j.decode(j.encode(value))
            assert(copy.items[1] == j.null and copy.items[3].name == value.items[3].name)
            assert(j.encode({}) == '{}')
            assert(j.encode({true, false, j.null}) == '[true,false,null]')
            local a = j.array(); a[1] = 'x'; a[1] = nil
            assert(j.encode(a) == '[]' and j.encode(j.object()) == '{}')
            assert(j.decode('9223372036854775807') == math.maxinteger)
            assert(j.decode('-9223372036854775808') == math.mininteger)
            assert(j.encode(math.maxinteger) == '9223372036854775807')
            assert(j.decode('1.25e2') == 125)
            assert(j.decode('"\\ud83d\\ude00"') == '😀')
            assert(j.decode(j.encode({['a' .. string.char(0) .. 'b'] = 7}))['a' .. string.char(0) .. 'b'] == 7)
            local function bad_decode(text)
                local ok = pcall(j.decode, text); assert(not ok, text)
            end
            for _, text in ipairs({'', '{', '[1,]', '{"a":1,"a":2}', '//comment\n{}',
                    '{} {}', '01', '-01', '+1', '1.', '.1', '1e', 'NaN', 'Infinity', '1e9999', '9223372036854775808',
                    '-9223372036854775809', '18446744073709551616000',
                    '"\\ud800"', '"\\udc00"', '[,1]', '{a:1}'}) do bad_decode(text) end
            bad_decode('"' .. string.char(0xc0, 0x80) .. '"')
            bad_decode('"raw' .. string.char(10) .. 'line"')
            bad_decode('{}' .. string.char(0) .. 'garbage')
            bad_decode(string.rep('[', 70) .. '0' .. string.rep(']', 70))
            bad_decode(string.rep(' ', 262145))
            bad_decode('[' .. string.rep('0,', 16384) .. '0]')
            local function bad_encode(value) assert(not pcall(j.encode, value)) end
            bad_encode(nil); bad_encode(function() end); bad_encode(0/0); bad_encode(math.huge)
            bad_encode({[2] = 'hole'}); bad_encode({[0] = 1}); bad_encode({a=1, [1]=2})
            bad_encode({[true] = 1}); bad_encode(setmetatable({}, {__index = function() error('called') end}))
            local cycle = {}; cycle.self = cycle; bad_encode(cycle)
            local wrong = j.array(); wrong.foo = true; bad_encode(wrong)
            local wrong_object = j.object(); wrong_object[1] = true; bad_encode(wrong_object)
            bad_encode(string.char(0xed, 0xa0, 0x80))
            bad_encode(string.rep('x', 262145))
            bad_encode(string.rep(string.char(0), 50000)) -- escaped output exceeds limit
            local deep = {}; local cursor = deep
            for i=1,70 do cursor.child = {}; cursor = cursor.child end
            bad_encode(deep)
            local child = {answer=42}; local shared = j.decode(j.encode({child, child}))
            assert(shared[1].answer == 42 and shared[2].answer == 42)
            assert(not pcall(j.decode, '{}', 1) and not pcall(j.encode))
            assert(not pcall(j.array, {}) and not pcall(j.object, {}))
            assert(j.decode('{"recovered":true}').recovered)
            return {decision='allow'}
        end
    )lua");
    policy.validate();
    check(std::holds_alternative<Allow>(policy.evaluate({{{}, "example"}, {}, {}})),
          "JSON round trips, strict errors, limits and recovery");
    std::vector<std::future<Decision>> futures;
    for (int i = 0; i < 4; ++i) futures.push_back(std::async(std::launch::async, [&policy] {
        return policy.evaluate({{{}, "example"}, {}, {}});
    }));
    for (auto& future : futures) check(std::holds_alternative<Allow>(future.get()), "isolated concurrent JSON calls");
    rejects("function preflight() sipi.json.decode('{'); return {decision='allow'} end",
            ErrorCode::script_execution, "uncaught JSON failure is a script error");
    const auto pressure = script(R"lua(
        function preflight()
            local text = '[' .. string.rep('{},', 16000) .. '{}]'
            local held = {}
            while pcall(function() held[#held+1] = string.rep('x', 200000) end) do end
            local ok = pcall(sipi.json.decode, text)
            assert(not ok) -- result construction exceeds the remaining Lua memory
            held = nil
            collectgarbage('collect')
            assert(#sipi.json.decode(text) == 16001)
            return {decision='allow'}
        end
    )lua");
    check(std::holds_alternative<Allow>(pressure.evaluate({{{}, "example"}, {}, {}})),
          "protected JSON result allocation failure and recovery");
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
        watermark_contract();
        resolution_contract();
        invalid_scripts_and_results();
        isolation_and_snapshot();
        library_contract();
        json_contract();
        startup_validation();
        resource_limits();
    } catch (const std::exception& error) {
        std::cerr << "Unexpected error: " << error.what() << '\n';
        return 1;
    }
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
