#include "preflight/LuaPreflight.h"

#include <lua.hpp>

#include <cstring>
#include <cstdlib>
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

/** Compile, execute, and validate inside one protected Lua boundary. Returns
 * two normalized values for evaluation, or none for validation (null request). */
int evaluate_protected(lua_State* state) {
    auto* evaluation = static_cast<Evaluation*>(lua_touserdata(state, 1));
    lua_settop(state, 0);
    open_libraries(state);

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

    evaluation->phase = ErrorCode::script_execution;
    lua_call(state, 1, LUA_MULTRET);
    evaluation->phase = ErrorCode::contract;
    if (lua_gettop(state) != 1 || lua_type(state, 1) != LUA_TTABLE) {
        return luaL_error(state, "preflight must return exactly one table");
    }
    if (lua_getmetatable(state, 1)) {
        return luaL_error(state, "preflight result must not have a metatable");
    }
    lua_pushnil(state);
    while (lua_next(state, 1)) {
        if (!is_string(state, -2, "decision") && !is_string(state, -2, "source")) {
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
        if (lua_type(state, 3) != LUA_TSTRING) {
            return luaL_error(state, "source must be a nonempty string without NUL bytes");
        }
        std::size_t length = 0;
        const char* source = lua_tolstring(state, 3, &length);
        if (length == 0 || std::memchr(source, '\0', length)) {
            return luaL_error(state, "source must be a nonempty string without NUL bytes");
        }
    }
    return 2;
}

} // namespace

PreflightError::PreflightError(ErrorCode code, const std::string& message)
    : std::runtime_error(message), code_(code) {}

LuaPreflight::LuaPreflight(const std::filesystem::path& script_path)
    : chunk_name_("@" + script_path.string()) {
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
    LuaState state{lua_newstate(allocate, &budget), &lua_close};
    Runtime() {
        if (!state) throw PreflightError(ErrorCode::memory, "cannot create Lua state");
        *static_cast<Budget**>(lua_getextraspace(state.get())) = &budget;
        lua_sethook(state.get(), instruction_limit, LUA_MASKCOUNT, 1000);
    }
};

/** Common protected boundary for startup validation and request evaluation. */
void execute(Runtime& runtime, const std::string& script, const std::string& chunk_name,
             const Request* request) {
    auto& state = runtime.state;
    Evaluation evaluation{&script, &chunk_name, request};

    // A zero-upvalue C function and light userdata do not allocate in Lua.
    // All allocating Lua operations happen inside the protected callback.
    lua_pushcfunction(state.get(), evaluate_protected);
    lua_pushlightuserdata(state.get(), &evaluation);
    const int status = lua_pcall(state.get(), 1, request ? 2 : 0, 0);
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
    Runtime runtime;
    execute(runtime, script_, chunk_name_, nullptr);
}

Decision LuaPreflight::evaluate(const Request& request) const {
    Runtime runtime;
    execute(runtime, script_, chunk_name_, &request);
    auto& state = runtime.state;
    if (is_string(state.get(), 1, "deny")) return Deny{};
    if (lua_isnil(state.get(), 2)) return Allow{std::nullopt};
    std::size_t length = 0;
    const char* source = lua_tolstring(state.get(), 2, &length);
    return Allow{std::filesystem::path(std::string(source, length))};
}

} // namespace sipi::preflight
