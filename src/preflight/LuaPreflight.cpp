#include "preflight/LuaPreflight.h"
#include "preflight/LuaJson.h"
#include "preflight/LuaHttp.h"
#include "preflight/LuaJwt.h"
#include "preflight/HttpClient.h"
#include "preflight/LuaCache.h"

#include <lua.hpp>

#include <cstring>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <fstream>
#include <iterator>
#include <memory>

namespace sipi::preflight {
namespace {

/** Borrowed only for the duration of lua_pcall. Lua callbacks update the phase
 * so the host can distinguish script failures from invalid return contracts. */
struct Evaluation {
    const std::string* script;
    const std::string* chunk_name;
    const Request* request;
    HttpSession* http;
    const ScriptConfig* config;
    CacheSession* cache;
    ErrorCode phase = ErrorCode::script_execution;
};

// Everything that can raise a Lua error runs beneath the outer lua_pcall.
// These callbacks intentionally have no nontrivial C++ automatic objects:
// a C-built Lua uses longjmp and cannot unwind C++ RAII objects.
void push_string(lua_State* state, const std::string& value) {
    lua_pushlstring(state, value.data(), value.size());
}

void push_optional_field(lua_State* state, const char* name,
                         const std::optional<std::string>& value) {
    if (value) {
        push_string(state, *value);
        lua_setfield(state, -2, name);
    }
}

/** Raw table access avoids executing a result table's __index metamethod. */
void raw_field(lua_State* state, int index, const char* name) {
    lua_pushstring(state, name);
    lua_rawget(state, index);
}

bool is_string(lua_State* state, int index, const char* expected) {
    if (lua_type(state, index) != LUA_TSTRING) return false;
    std::size_t length = 0;
    const char* value = lua_tolstring(state, index, &length);
    return length == std::strlen(expected) && std::memcmp(value, expected, length) == 0;
}

/** Validate a required source while retaining the caller's stack. Only trivial
 * locals may cross Lua errors under the protected call. */
void check_source(lua_State* state, int index) {
    const int top = lua_gettop(state);
    int value = index;
    if (lua_type(state,index) == LUA_TTABLE) {
        if (lua_getmetatable(state,index)) luaL_error(state,"source must not have a metatable");
        lua_pushnil(state);
        while (lua_next(state,index)) {
            if (!is_string(state,-2,"type") && !is_string(state,-2,"key"))
                luaL_error(state,"unknown field in source");
            lua_pop(state,1);
        }
        raw_field(state,index,"type");
        if (!is_string(state,-1,"s3")) luaL_error(state,"source type must be s3");
        lua_pop(state,1);
        raw_field(state,index,"key");
        value = lua_gettop(state);
    }
    if (lua_type(state,value) != LUA_TSTRING) luaL_error(state,"source path/key must be a string");
    std::size_t length = 0;
    const char* text = lua_tolstring(state,value,&length);
    if (!length || std::memchr(text,'\0',length)) luaL_error(state,"source path/key must be nonempty without NUL bytes");
    lua_settop(state,top);
}

/** Strict plain-table contract; all checks run beneath lua_pcall. */
void check_resolution(lua_State* state, int index) {
    if (lua_type(state,index) != LUA_TTABLE || lua_getmetatable(state,index))
        luaL_error(state,"resolution must be a plain table");
    lua_pushnil(state);
    while (lua_next(state,index)) {
        if (!is_string(state,-2,"max_width") && !is_string(state,-2,"max_height"))
            luaL_error(state,"unknown field in resolution");
        lua_pop(state,1);
    }
    for (const char* name : {"max_width", "max_height"}) {
        raw_field(state,index,name);
        if (!lua_isinteger(state,-1) || lua_tointeger(state,-1) < 1 ||
            lua_tointeger(state,-1) > std::numeric_limits<std::uint32_t>::max())
            luaL_error(state,"resolution axes must be positive uint32 integers");
        lua_pop(state,1);
    }
}

void check_watermark(lua_State* state, int index) {
    if (lua_type(state,index) != LUA_TTABLE || lua_getmetatable(state,index))
        luaL_error(state,"watermark must be a plain table");
    lua_pushnil(state);
    while (lua_next(state,index)) {
        if (!is_string(state,-2,"source") && !is_string(state,-2,"width_fraction") &&
            !is_string(state,-2,"opacity") && !is_string(state,-2,"color"))
            luaL_error(state,"unknown field in watermark");
        lua_pop(state,1);
    }
    raw_field(state,index,"source"); check_source(state,lua_gettop(state)); lua_pop(state,1);
    raw_field(state,index,"width_fraction");
    const double width = lua_tonumber(state,-1);
    if (lua_type(state,-1) != LUA_TNUMBER || !std::isfinite(width) || width <= 0 || width > 1)
        luaL_error(state,"watermark width_fraction must be a number in (0,1]");
    lua_pop(state,1);
    raw_field(state,index,"opacity");
    const double opacity = lua_tonumber(state,-1);
    if (lua_type(state,-1) != LUA_TNUMBER || !std::isfinite(opacity) || opacity < 0 || opacity > 1)
        luaL_error(state,"watermark opacity must be a number in [0,1]");
    lua_pop(state,1);
    raw_field(state,index,"color");
    const int color = lua_gettop(state);
    if (lua_type(state,color) != LUA_TTABLE || lua_getmetatable(state,color))
        luaL_error(state,"watermark color must be a plain RGB array");
    unsigned count = 0;
    lua_pushnil(state);
    while (lua_next(state,color)) {
        if (!lua_isinteger(state,-2) || lua_tointeger(state,-2)<1 || lua_tointeger(state,-2)>3 ||
            !lua_isinteger(state,-1) || lua_tointeger(state,-1)<0 || lua_tointeger(state,-1)>255)
            luaL_error(state,"watermark color requires three integer samples in [0,255]");
        ++count;
        lua_pop(state,1);
    }
    if (count != 3) luaL_error(state,"watermark color requires exactly three samples");
    lua_pop(state,1);
}




/** Minimal standard facilities for trusted policy scripts. Deliberately omit
 * io/os/package/debug and remove base-library file loading and console output.
 * This reduces incidental side effects; it is not a security sandbox. */
void open_libraries(lua_State* state) {
    luaL_requiref(state, LUA_GNAME, luaopen_base, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(state, 1);
    luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(state, 1);
    for (const char* name : {"dofile", "loadfile", "load", "print", "warn"}) {
        lua_pushnil(state);
        lua_setglobal(state, name);
    }
}

/** Fresh tables keep script mutation local to one evaluation. All allocating
 * operations run inside the outer protected boundary; the map is borrowed. */
void push_settings(lua_State* state, const ScriptMap& values) {
    lua_newtable(state);
    for (const auto& [name, value] : values) {
        push_string(state, name);
        push_string(state, value);
        lua_rawset(state, -3);
    }
}

/** Compile, execute, and validate inside one protected Lua boundary. Returns
 * four normalized values for evaluation, or none for validation (null request). */
int evaluate_protected(lua_State* state) {
    auto* evaluation = static_cast<Evaluation*>(lua_touserdata(state, 1));
    lua_settop(state, 0);
    open_libraries(state);
    open_json(state);
    open_http(state, *evaluation->http);
    open_jwt(state);
    open_cache(state, *evaluation->cache);
    lua_getglobal(state, "sipi");
    push_settings(state, evaluation->config->config);
    lua_setfield(state, -2, "config");
    push_settings(state, evaluation->config->secrets);
    lua_setfield(state, -2, "secrets");
    lua_pop(state, 1);

    evaluation->phase = ErrorCode::script_load;
    if (luaL_loadbufferx(state, evaluation->script->data(), evaluation->script->size(),
                         evaluation->chunk_name->c_str(), "t") != LUA_OK) {
        return lua_error(state);
    }
    evaluation->phase = ErrorCode::script_execution;
    lua_call(state, 0, 0);

    evaluation->phase = ErrorCode::contract;
    lua_pushglobaltable(state);
    raw_field(state, 1, "preflight");
    lua_remove(state, 1);
    if (lua_type(state, 1) != LUA_TFUNCTION) {
        return luaL_error(state, "script must define a preflight function");
    }

    if (!evaluation->request) return 0;

    lua_newtable(state);
    lua_newtable(state);
    for (std::size_t i = 0; i < evaluation->request->resource.prefix.size(); ++i) {
        push_string(state, evaluation->request->resource.prefix[i]);
        lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
    }
    lua_setfield(state, -2, "prefix");
    push_string(state, evaluation->request->resource.identifier);
    lua_setfield(state, -2, "identifier");
    lua_newtable(state);
    push_optional_field(state, "authorization", evaluation->request->authorization);
    push_optional_field(state, "cookie", evaluation->request->cookie);
    lua_setfield(state, -2, "headers");
    lua_newtable(state);
    for (const auto& [name, value] : evaluation->request->cookies) {
        push_string(state, name);
        push_string(state, value);
        lua_rawset(state, -3);
    }
    lua_setfield(state, -2, "cookies");
    lua_newtable(state);
    for (const auto& [name, values] : evaluation->request->query) {
        push_string(state, name);
        lua_newtable(state);
        for (std::size_t i = 0; i < values.size(); ++i) {
            push_string(state, values[i]);
            lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
        }
        lua_rawset(state, -3);
    }
    lua_setfield(state, -2, "query");
    const char* kind = "image";
    switch (evaluation->request->kind) {
        case RequestKind::image: break;
        case RequestKind::info: kind = "info"; break;
        case RequestKind::base: kind = "base"; break;
    }
    lua_pushstring(state, kind);
    lua_setfield(state, -2, "kind");
    lua_pushstring(state, evaluation->request->method == RequestMethod::head ? "HEAD" : "GET");
    lua_setfield(state, -2, "method");

    evaluation->phase = ErrorCode::script_execution;
    evaluation->http->enable();
    evaluation->cache->enabled = true;
    lua_call(state, 1, LUA_MULTRET);
    evaluation->http->disable();
    evaluation->cache->enabled = false;
    evaluation->phase = ErrorCode::contract;
    if (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TTABLE) {
        return luaL_error(state, "preflight must return exactly one table");
    }
    if (lua_getmetatable(state, 1)) {
        return luaL_error(state, "preflight result must not have a metatable");
    }
    lua_pushnil(state);
    while (lua_next(state, 1)) {
        if (!is_string(state, -2, "decision") && !is_string(state, -2, "source") && !is_string(state, -2, "watermark") && !is_string(state, -2, "resolution")) {
            return luaL_error(state, "unknown field in preflight result");
        }
        lua_pop(state, 1);
    }

    raw_field(state, 1, "decision");
    const bool allowed = is_string(state, 2, "allow");
    if (!allowed && !is_string(state, 2, "deny")) {
        return luaL_error(state, "decision must be 'allow' or 'deny'");
    }
    raw_field(state, 1, "source");
    if (!lua_isnil(state, 3)) {
        if (!allowed) return luaL_error(state, "deny must not include a source");
        check_source(state,3);
    }
    raw_field(state,1,"watermark");
    if (!lua_isnil(state,4)) {
        if (!allowed) return luaL_error(state,"deny must not include a watermark");
        check_watermark(state,4);
    }
    raw_field(state,1,"resolution");
    if (!lua_isnil(state,5)) {
        if (!allowed) return luaL_error(state,"deny must not include a resolution");
        check_resolution(state,5);
    }
    return 4;
}

} // namespace

PreflightError::PreflightError(ErrorCode code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

LuaPreflight::LuaPreflight(const std::filesystem::path& script_path, ScriptConfig config)
    : chunk_name_("@" + script_path.string()), config_(std::move(config)), cache_(std::make_shared<SharedCache>()) {
    validate_script_config(config_);
    std::ifstream input(script_path, std::ios::binary);
    if (!input) throw PreflightError(ErrorCode::script_file, "cannot open preflight script");
    try {
        script_.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    } catch (const std::ios_base::failure&) {
        throw PreflightError(ErrorCode::script_file, "cannot read preflight script");
    }
    if (input.bad()) throw PreflightError(ErrorCode::script_file, "cannot read preflight script");
}

namespace {
using LuaState = std::unique_ptr<lua_State, decltype(&lua_close)>;

/** Limits apply to Lua allocations and VM instructions, including initialization.
 * They do not preempt native functions or filesystem operations. */
struct Budget {
    static constexpr std::size_t memory_limit = 16 * 1024 * 1024;
    std::size_t bytes = 0;
    int remaining_hooks = 1000; // 1000 instructions per hook: one million total.
    bool exhausted = false;
};

/** Lua allocator contract: old_size is a type tag when pointer is null. */
void* allocate(void* context, void* pointer, std::size_t old_size, std::size_t new_size) noexcept {
    auto& budget = *static_cast<Budget*>(context);
    if (!pointer) old_size = 0;
    if (new_size == 0) {
        std::free(pointer);
        budget.bytes -= old_size;
        return nullptr;
    }
    if (new_size > Budget::memory_limit - (budget.bytes - old_size)) return nullptr;
    void* replacement = std::realloc(pointer, new_size);
    if (replacement) budget.bytes = budget.bytes - old_size + new_size;
    return replacement;
}

void instruction_limit(lua_State* state, lua_Debug*) {
    auto* budget = *static_cast<Budget**>(lua_getextraspace(state));
    if (--budget->remaining_hooks <= 0) {
        budget->exhausted = true;
        // A caught error does not replenish the budget. Check each subsequent
        // instruction so scripts cannot continue via pcall after exhaustion.
        lua_sethook(state, instruction_limit, LUA_MASKCOUNT, 1);
        luaL_error(state, "preflight instruction budget exceeded");
    }
}

/** Member order keeps allocator bookkeeping alive through lua_close. */
struct Runtime {
    Budget budget;
    HttpSession http; // Must outlive Lua-state finalizers.
    CacheSession cache; // Also outlives Lua finalizers.
    LuaState state{lua_newstate(allocate, &budget), &lua_close};
    explicit Runtime(SharedCache& shared) : cache{shared, false, false, 0, {}} {
        if (!state) throw PreflightError(ErrorCode::memory, "cannot create Lua state");
        *static_cast<Budget**>(lua_getextraspace(state.get())) = &budget;
        lua_sethook(state.get(), instruction_limit, LUA_MASKCOUNT, 1000);
    }
};

/** Common protected boundary for startup validation and request evaluation. */
void execute(Runtime& runtime, const std::string& script, const std::string& chunk_name,
             const Request* request, const ScriptConfig& config) {
    auto& state = runtime.state;
    Evaluation evaluation{&script, &chunk_name, request, &runtime.http, &config, &runtime.cache};

    // A zero-upvalue C function and light userdata do not allocate in Lua.
    // All allocating Lua operations happen inside the protected callback.
    lua_pushcfunction(state.get(), evaluate_protected);
    lua_pushlightuserdata(state.get(), &evaluation);
    const int status = lua_pcall(state.get(), 1, request ? 4 : 0, 0);
    runtime.http.disable(); // Also on Lua errors; finalizers cannot initiate requests.
    runtime.cache.enabled = false;
    if (runtime.budget.exhausted) {
        throw PreflightError(ErrorCode::instruction_limit, "preflight instruction budget exceeded");
    }
    if (status != LUA_OK) {
        std::size_t length = 0;
        const char* message = lua_type(state.get(), -1) == LUA_TSTRING
            ? lua_tolstring(state.get(), -1, &length) : nullptr;
        throw PreflightError(status == LUA_ERRMEM ? ErrorCode::memory : evaluation.phase,
                             message ? std::string(message, length)
                                     : "Lua raised a non-string error");
    }

}
} // namespace

void LuaPreflight::validate() const {
    Runtime runtime(*cache_);
    execute(runtime, script_, chunk_name_, nullptr, config_);
}

Decision LuaPreflight::evaluate(const Request& request) const {
    Runtime runtime(*cache_);
    execute(runtime, script_, chunk_name_, &request, config_);
    auto& state = runtime.state;
    if (is_string(state.get(), 1, "deny")) return Deny{};
    // Contract checks ran under lua_pcall. Field names already exist; no
    // metamethod is invoked. Construct owning native values after that boundary.
    const auto source_at = [&](int index) -> SourceOverride {
        std::size_t length = 0;
        if (lua_type(state.get(),index) == LUA_TTABLE) {
            raw_field(state.get(),index,"key");
            const char* text = lua_tolstring(state.get(),-1,&length);
            S3Key key{std::string(text,length)};
            lua_pop(state.get(),1);
            return key;
        }
        const char* text = lua_tolstring(state.get(),index,&length);
        return std::filesystem::path(std::string(text,length));
    };
    Allow result{std::nullopt};
    if (!lua_isnil(state.get(),2)) result.source = source_at(2);
    if (!lua_isnil(state.get(),3)) {
        raw_field(state.get(),3,"source");
        auto source = source_at(lua_gettop(state.get()));
        lua_pop(state.get(),1);
        raw_field(state.get(),3,"width_fraction");
        const double width = lua_tonumber(state.get(),-1); lua_pop(state.get(),1);
        raw_field(state.get(),3,"opacity");
        const double opacity = lua_tonumber(state.get(),-1); lua_pop(state.get(),1);
        raw_field(state.get(),3,"color");
        std::array<std::uint8_t,3> color{};
        for (int i=0;i<3;++i) {
            lua_rawgeti(state.get(),5,i+1);
            color[i] = static_cast<std::uint8_t>(lua_tointeger(state.get(),-1));
            lua_pop(state.get(),1);
        }
        result.watermark = Watermark{std::move(source),width,opacity,color};
    }
    lua_settop(state.get(),4);
    if (!lua_isnil(state.get(),4)) {
        raw_field(state.get(),4,"max_width");
        const auto width = static_cast<std::uint32_t>(lua_tointeger(state.get(),-1));
        lua_pop(state.get(),1);
        raw_field(state.get(),4,"max_height");
        const auto height = static_cast<std::uint32_t>(lua_tointeger(state.get(),-1));
        result.resolution = ResolutionLimit{width,height};
    }
    return result;
}

} // namespace sipi::preflight
