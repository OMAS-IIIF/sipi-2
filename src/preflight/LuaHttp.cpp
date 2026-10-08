#include "preflight/LuaHttp.h"
#include "preflight/HttpClient.h"

#include <lua.hpp>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace sipi::preflight {
namespace {
void require(bool value, const char* message) {
    if (!value) throw std::invalid_argument(message);
}
std::string text(lua_State* state, int index, std::size_t limit) {
    require(lua_type(state, index) == LUA_TSTRING, "HTTP expects string values");
    std::size_t size;
    const char* value = lua_tolstring(state, index, &size); // String already; no coercion/allocation.
    require(size <= limit, "HTTP string exceeds limit");
    return {value, size};
}
void plain_table(lua_State* state, int index) {
    require(lua_type(state, index) == LUA_TTABLE, "HTTP expects a plain table");
    if (lua_getmetatable(state, index)) {
        lua_pop(state, 1);
        throw std::invalid_argument("HTTP tables must not have metatables");
    }
}
long integer(lua_State* state, int index, long maximum) {
    require(lua_isinteger(state, index), "HTTP limits must be integers");
    const auto value = lua_tointeger(state, index);
    require(value >= 1 && value <= maximum, "HTTP limit outside allowed range");
    return static_cast<long>(value);
}

/** Raw reads only, after the caller has reserved stack space. Validate header
 * shapes before transport; unknown fields fail rather than silently misspelling
 * authentication or timeout options. */
HttpHeaders read_headers(lua_State* state, int index) {
    plain_table(state, index);
    index = lua_absindex(state, index);
    HttpHeaders headers;
    std::size_t bytes = 0, count = 0;
    lua_pushnil(state);
    while (lua_next(state, index)) {
        auto name = text(state, -2, 256);
        for (auto& c : name) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        require(!headers.contains(name), "Duplicate case-insensitive HTTP header name");
        std::vector<std::string> values;
        if (lua_type(state, -1) == LUA_TSTRING) values.push_back(text(state, -1, 65536));
        else {
            plain_table(state, -1);
            const int table = lua_absindex(state, -1);
            std::size_t entries = 0, maximum = 0;
            lua_pushnil(state);
            while (lua_next(state, table)) {
                const auto key = static_cast<std::size_t>(integer(state, -2, 128));
                if (values.size() < key) values.resize(key);
                values[key - 1] = text(state, -1, 65536);
                maximum = std::max(maximum, key);
                require(++entries <= 128, "Too many HTTP header values");
                lua_pop(state, 1);
            }
            require(entries > 0 && entries == maximum, "HTTP header lists must be nonempty dense arrays");
        }
        for (const auto& value : values) {
            require(++count <= 128 && name.size() + value.size() + 4 <= 65536 - bytes,
                    "HTTP request headers exceed limit");
            bytes += name.size() + value.size() + 4;
        }
        headers.emplace(std::move(name), std::move(values));
        lua_pop(state, 1);
    }
    return headers;
}
HttpRequest read_request(lua_State* state) {
    require(lua_gettop(state) == 1, "http.request expects exactly one options table");
    plain_table(state, 1);
    HttpRequest request;
    lua_pushnil(state);
    while (lua_next(state, 1)) {
        const auto name = text(state, -2, 64);
        if (name == "url") request.url = text(state, -1, 8192);
        else if (name == "method") request.method = text(state, -1, 16);
        else if (name == "headers") request.headers = read_headers(state, -1);
        else if (name == "body") request.body = text(state, -1, 1024 * 1024);
        else if (name == "ca_file") request.ca_file = text(state, -1, 4096);
        else if (name == "timeout_ms") request.timeout_ms = integer(state, -1, 10000);
        else if (name == "connect_timeout_ms") request.connect_timeout_ms = integer(state, -1, 2000);
        else if (name == "max_response_bytes") request.max_response_bytes = static_cast<std::size_t>(integer(state, -1, 1024 * 1024));
        else throw std::invalid_argument("Unknown HTTP request option");
        lua_pop(state, 1);
    }
    return request;
}

/** Only borrowed references/trivial iterators exist across allocating Lua calls. */
void push_headers(lua_State* state, const HttpHeaders& headers) {
    lua_newtable(state);
    for (const auto& [name, values] : headers) {
        lua_pushlstring(state, name.data(), name.size());
        lua_newtable(state);
        for (std::size_t i = 0; i < values.size(); ++i) {
            lua_pushlstring(state, values[i].data(), values[i].size());
            lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1));
        }
        lua_rawset(state, -3);
    }
}
int push_result(lua_State* state) {
    const auto* result = static_cast<const HttpResult*>(lua_touserdata(state, 1));
    lua_settop(state, 0);
    if (result->failure) {
        lua_pushnil(state);
        lua_newtable(state);
        lua_pushstring(state, result->failure->code); lua_setfield(state, -2, "code");
        lua_pushstring(state, result->failure->message); lua_setfield(state, -2, "message");
        return 2;
    }
    lua_newtable(state);
    lua_pushinteger(state, result->response.status); lua_setfield(state, -2, "status");
    lua_pushlstring(state, result->response.body.data(), result->response.body.size()); lua_setfield(state, -2, "body");
    push_headers(state, result->response.headers); lua_setfield(state, -2, "headers");
    push_headers(state, result->response.trailers); lua_setfield(state, -2, "trailers");
    lua_pushnil(state);
    return 2;
}
int request(lua_State* state) {
    luaL_checkstack(state, 16, "HTTP Lua stack exhausted"); // Before constructing C++ owners.
    auto* session = static_cast<HttpSession*>(lua_touserdata(state, lua_upvalueindex(1)));
    int status = LUA_OK;
    char error[192]{};
    try {
        const auto input = read_request(state);
        const auto result = session->request(input);
        lua_pushcfunction(state, push_result);
        lua_pushlightuserdata(state, const_cast<HttpResult*>(&result));
        status = lua_pcall(state, 1, 2, 0);
    } catch (const std::exception& exception) {
        std::strncpy(error, exception.what(), sizeof(error) - 1);
    } catch (...) {
        std::strcpy(error, "HTTP request failed");
    }
    // No native owners survive either Lua error path.
    if (error[0]) return luaL_error(state, "%s", error);
    if (status != LUA_OK) return lua_error(state);
    return 2;
}
}
void open_http(lua_State* state, HttpSession& session) {
    lua_getglobal(state, "sipi");
    lua_newtable(state);
    lua_pushlightuserdata(state, &session);
    lua_pushcclosure(state, request, 1);
    lua_setfield(state, -2, "request");
    lua_setfield(state, -2, "http");
    lua_pop(state, 1);
}
}
