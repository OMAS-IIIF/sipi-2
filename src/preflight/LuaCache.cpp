#include "LuaCache.h"
#include "LuaJson.h"
#include <json/json.h>
#include <lua.hpp>
#include <stdexcept>
#include <cstring>

namespace sipi::preflight {
namespace {
void require(bool condition) {
    if (!condition) throw std::invalid_argument("Invalid cache operation");
}
std::string key(lua_State* state) {
    require(lua_type(state, 1) == LUA_TSTRING);
    std::size_t length = 0;
    const char* value = lua_tolstring(state, 1, &length);
    require(length > 0 && length <= 256);
    return {value, length};
}
CacheSession& session(lua_State* state) {
    auto& result = *static_cast<CacheSession*>(lua_touserdata(state, lua_upvalueindex(1)));
    require(result.enabled && ++result.operations <= 64);
    return result;
}
/** Unwind C++ cache/loader owners while retaining the original Lua error on
 * the stack. Rethrow it into Lua only after leaving all C++ catch scopes. */
struct LoaderFailure {};

/** All callbacks are protected with lua_pcall before control crosses native owners.
 * The guard resets even on loader/JSON/native failures; no Lua longjmp crosses it. */
SharedCache::Loaded load(lua_State* state, CacheSession& context) {
    struct Loading {
        bool& flag;
        explicit Loading(bool& value) : flag(value) { flag = true; }
        ~Loading() { flag = false; }
    } guard(context.loading);
    lua_pushvalue(state, 2); // Reserved stack; copies the loader argument.
    if (lua_pcall(state, 0, LUA_MULTRET, 0) != LUA_OK)
        throw LoaderFailure{};
    if (lua_gettop(state) != 4 || lua_isnil(state, 3) || !lua_isinteger(state, 4))
        throw std::invalid_argument("Cache loader must return a non-nil value and integer TTL (exactly two results)");
    const auto ttl = lua_tointeger(state, 4);
    if (ttl < 0 || ttl > 3600) throw std::invalid_argument("Cache TTL must be in 0..3600 seconds");
    auto bytes = write_json(read_json(state, 3));
    if (bytes.size() > 65536) throw std::invalid_argument("Cache value exceeds 64 KiB");
    lua_settop(state, 2);
    return {std::move(bytes), std::chrono::seconds(ttl)};
}
/** push_json provides an inner protected boundary; native result owners are
 * destroyed before the caller propagates an allocation/conversion error. */
int push_value(lua_State* state, const SharedCache::Value& value) {
    const auto decoded = parse_json(value.bytes);
    const int status = push_json(state, decoded);
    if (status == LUA_OK) lua_pushinteger(state, value.revision);
    return status;
}
int get(lua_State* state) {
    if (!lua_checkstack(state, 16)) return luaL_error(state, "Cache stack exhausted");
    bool failed = false;
    int status = LUA_OK;
    try {
        auto& context = session(state);
        require(lua_gettop(state) == 1);
        const auto value = context.cache.get(key(state));
        if (value) status = push_value(state, *value);
        else { lua_pushnil(state); lua_pushnil(state); }
    } catch (...) { failed = true; }
    if (failed) return luaL_error(state, "Cache get failed (arguments, limits or native failure)");
    if (status != LUA_OK) return lua_error(state);
    return 2;
}
int get_or_load(lua_State* state) {
    if (!lua_checkstack(state, 16)) return luaL_error(state, "Cache stack exhausted");
    bool loader_failed = false;
    char failure[512]{}; // Trivial storage: luaL_error must not skip native destructors.
    int status = LUA_OK;
    try {
        auto& context = session(state);
        require(lua_gettop(state) == 2 && lua_type(state, 2) == LUA_TFUNCTION);
        if (context.loading) throw std::invalid_argument("Nested cache loads are forbidden");
        if (!context.deadline) context.deadline = SharedCache::Clock::now() + std::chrono::seconds(5);
        const auto value = context.cache.get_or_load(key(state), [&] { return load(state, context); }, *context.deadline);
        status = push_value(state, value);
    } catch (const LoaderFailure&) {
        loader_failed = true;
    } catch (const std::exception& error) {
        // Native messages contain fixed diagnostic text, never keys or values.
        std::strncpy(failure, error.what(), sizeof(failure) - 1);
    } catch (...) {
        std::strncpy(failure, "Unknown native failure", sizeof(failure) - 1);
    }
    // SharedCache has already removed the failed flight and notified waiters.
    // The outer preflight boundary masks credentials in the original Lua message.
    if (loader_failed) return lua_error(state);
    if (failure[0]) return luaL_error(state, "Cache load failed: %s", failure);
    if (status != LUA_OK) return lua_error(state);
    return 2;
}
int invalidate(lua_State* state) {
    if (!lua_checkstack(state, 8)) return luaL_error(state, "Cache stack exhausted");
    bool failed = false;
    bool removed = false;
    try {
        auto& context = session(state);
        require(lua_gettop(state) == 2 && lua_isinteger(state, 2) && lua_tointeger(state, 2) > 0);
        removed = context.cache.invalidate(key(state), lua_tointeger(state, 2));
    } catch (...) { failed = true; }
    if (failed) return luaL_error(state, "Cache invalidation failed (arguments, limits or native failure)");
    lua_pushboolean(state, removed);
    return 1;
}
}
void open_cache(lua_State* state, CacheSession& context) {
    lua_getglobal(state, "sipi");
    lua_newtable(state);
    static constexpr luaL_Reg functions[] = {
        {"get", get}, {"get_or_load", get_or_load}, {"invalidate", invalidate}
    };
    for (const auto& function : functions) {
        lua_pushlightuserdata(state, &context);
        lua_pushcclosure(state, function.func, 1);
        lua_setfield(state, -2, function.name);
    }
    lua_setfield(state, -2, "cache");
    lua_pop(state, 1);
}
}
