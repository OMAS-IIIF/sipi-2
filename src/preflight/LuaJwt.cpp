#include "preflight/LuaJwt.h"
#include "preflight/Jwt.h"
#include "preflight/LuaJson.h"

#include <lua.hpp>

#include <chrono>
#include <cstring>
#include <stdexcept>

namespace sipi::preflight {
namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::invalid_argument(message);
}
std::string text(lua_State* state, int index, std::size_t limit) {
    require(lua_type(state, index) == LUA_TSTRING, "JWT expects a string");
    std::size_t size;
    const char* value = lua_tolstring(state, index, &size);
    require(size <= limit, "JWT string exceeds limit");
    return {value, size};
}
JwtOptions read_options(lua_State* state, bool signing) {
    require(lua_gettop(state) == 2 && lua_type(state, 2) == LUA_TTABLE, "JWT expects value and options table");
    if (lua_getmetatable(state, 2)) {
        lua_pop(state, 1);
        throw std::invalid_argument("JWT options must be a plain table");
    }
    JwtOptions options;
    lua_pushnil(state);
    while (lua_next(state, 2)) {
        const auto name = text(state, -2, 64);
        if (name == "algorithm") options.algorithm = text(state, -1, 16);
        else if (name == "key") options.key = text(state, -1, 16384);
        else if (name == "type") options.type = text(state, -1, 1024);
        else if (name == "issuer" && !signing) options.issuer = text(state, -1, 1024);
        else if (name == "audience" && !signing) options.audience = text(state, -1, 1024);
        else if (name == "kid" && signing) options.kid = text(state, -1, 1024);
        else if (name == "require_exp" && !signing) {
            require(lua_type(state, -1) == LUA_TBOOLEAN, "JWT require_exp must be boolean");
            options.require_exp = lua_toboolean(state, -1) != 0;
        } else if ((name == "expires_in" && signing) || (name == "leeway" && !signing)) {
            require(lua_isinteger(state, -1), "JWT times must be integers");
            if (signing) options.expires_in = lua_tointeger(state, -1);
            else options.leeway = lua_tointeger(state, -1);
        } else throw std::invalid_argument("Unknown JWT option for this operation");
        lua_pop(state, 1);
    }
    return options;
}
int push_string(lua_State* state) {
    const auto* value = static_cast<const std::string*>(lua_touserdata(state, 1));
    lua_pushlstring(state, value->data(), value->size());
    return 1;
}
int push_failure(lua_State* state) {
    const auto* value = static_cast<const JwtFailure*>(lua_touserdata(state, 1));
    lua_pushnil(state);
    lua_newtable(state);
    lua_pushstring(state, value->code); lua_setfield(state, -2, "code");
    lua_pushstring(state, value->message); lua_setfield(state, -2, "message");
    return 2;
}

/** No native owner crosses a throwing Lua API. In particular, JSON results and
 * crypto temporaries are destroyed before propagating protected Lua errors. */
int run(lua_State* state, bool signing) {
    luaL_checkstack(state, 16, "JWT Lua stack exhausted");
    auto* count = static_cast<unsigned*>(lua_touserdata(state, lua_upvalueindex(1)));
    if (*count >= 32) return luaL_error(state, "JWT operation budget exhausted");
    ++*count;
    char error[192]{};
    int status = LUA_OK;
    try {
        const auto options = read_options(state, signing);
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (signing) {
            const auto claims = read_json(state, 1);
            const auto token = sign_jwt(claims, options, now);
            lua_pushcfunction(state, push_string);
            lua_pushlightuserdata(state, const_cast<std::string*>(&token));
            status = lua_pcall(state, 1, 1, 0);
        } else {
            // Bound native copies even when the incoming Lua token is enormous.
            require(lua_type(state, 1) == LUA_TSTRING, "JWT token must be a string");
            std::size_t size;
            const char* bytes = lua_tolstring(state, 1, &size);
            const auto token = size <= 16384 ? std::string(bytes, size) : std::string{};
            const auto result = verify_jwt(token, options, now);
            if (result.failure) {
                lua_pushcfunction(state, push_failure);
                lua_pushlightuserdata(state, const_cast<JwtFailure*>(&*result.failure));
                status = lua_pcall(state, 1, 2, 0);
            } else {
                status = push_json(state, result.claims);
                if (status == LUA_OK) lua_pushnil(state);
            }
        }
    } catch (const std::exception& exception) {
        std::strncpy(error, exception.what(), sizeof(error) - 1);
    } catch (...) {
        std::strcpy(error, "JWT operation failed");
    }
    if (error[0]) return luaL_error(state, "%s", error);
    if (status != LUA_OK) return lua_error(state);
    return signing ? 1 : 2;
}
int sign(lua_State* state) { return run(state, true); }
int verify(lua_State* state) { return run(state, false); }
}
void open_jwt(lua_State* state) {
    lua_getglobal(state, "sipi");
    lua_newtable(state);
    auto* count = static_cast<unsigned*>(lua_newuserdatauv(state, sizeof(unsigned), 0));
    *count = 0;
    lua_pushvalue(state, -1);
    lua_pushcclosure(state, sign, 1); lua_setfield(state, -3, "sign");
    lua_pushcclosure(state, verify, 1); lua_setfield(state, -2, "verify");
    lua_setfield(state, -2, "jwt");
    lua_pop(state, 1);
}
}
