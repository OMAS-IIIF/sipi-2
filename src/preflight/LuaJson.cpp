#include "preflight/LuaJson.h"

#include <lua.hpp>
#include <json/json.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sipi::preflight {
namespace {
constexpr std::size_t byte_limit = 256 * 1024;
constexpr std::size_t node_limit = 16384;
constexpr unsigned depth_limit = 64;
constexpr const char* array_tag = "sipi.json.array";
constexpr const char* object_tag = "sipi.json.object";
char null_identity;

[[noreturn]] void invalid(const char* reason) { throw std::runtime_error(reason); }

/** Reject malformed UTF-8, including surrogate code points and overlong forms. */
void utf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 128) continue;
        unsigned count, value, minimum;
        if (first >= 0xc2 && first <= 0xdf) { count = 1; value = first & 31; minimum = 128; }
        else if (first >= 0xe0 && first <= 0xef) { count = 2; value = first & 15; minimum = 2048; }
        else if (first >= 0xf0 && first <= 0xf4) { count = 3; value = first & 7; minimum = 65536; }
        else invalid("invalid UTF-8");
        if (count > text.size() - i) invalid("invalid UTF-8");
        while (count--) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0) != 0x80) invalid("invalid UTF-8");
            value = (value << 6) | (next & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            invalid("invalid UTF-8");
    }
}

/** JsonCpp may fall back to double for oversized integer tokens. Reject those
 * before parsing so integer identifiers never silently lose precision. */
void validate_tokens(std::string_view text) {
    bool quoted = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (static_cast<unsigned char>(c) < 32 && (quoted || (c != ' ' && c != '\t' && c != '\r' && c != '\n')))
            invalid("unescaped JSON control character");
        if (quoted && c == '\\') { ++i; continue; }
        if (c == '"') { quoted = !quoted; continue; }
        if (!quoted && (c == '+' || c == '.')) invalid("invalid JSON number");
        if (quoted || (c != '-' && (c < '0' || c > '9'))) continue;
        const auto start = i;
        bool real = false;
        while (i < text.size() && std::strchr("0123456789-eE+.", text[i]) && text[i] != '\0') {
            real |= text[i] == '.' || text[i] == 'e' || text[i] == 'E';
            ++i;
        }
        // JsonCpp accepts some non-JSON spellings (for example 01 and 1.).
        // Validate the token grammar explicitly before trusting its conversion.
        auto p = start;
        if (text[p] == '-') ++p;
        const auto digit = [&](std::size_t at) { return at < i && text[at] >= '0' && text[at] <= '9'; };
        if (!digit(p)) invalid("invalid JSON number");
        if (text[p] == '0') ++p;
        else while (digit(p)) ++p;
        if (p < i && text[p] == '.') {
            ++p;
            if (!digit(p)) invalid("invalid JSON number");
            while (digit(p)) ++p;
        }
        if (p < i && (text[p] == 'e' || text[p] == 'E')) {
            ++p;
            if (p < i && (text[p] == '+' || text[p] == '-')) ++p;
            if (!digit(p)) invalid("invalid JSON number");
            while (digit(p)) ++p;
        }
        if (p != i) invalid("invalid JSON number");
        if (!real) {
            lua_Integer number;
            const auto result = std::from_chars(text.data() + start, text.data() + i, number);
            if (result.ec != std::errc{} || result.ptr != text.data() + i)
                invalid("JSON integer exceeds signed 64-bit range");
        }
        --i;
    }
}

struct Budget {
    std::size_t nodes = 0;
    std::size_t bytes = 0;
    void visit(unsigned depth) {
        if (depth > depth_limit || ++nodes > node_limit) invalid("JSON structure limit exceeded");
    }
    void string(std::string_view value) {
        utf8(value);
        if (value.size() > byte_limit - bytes) invalid("JSON string budget exceeded");
        bytes += value.size();
    }
};

/** Decode is prepared as a linear stack program before entering Lua. This keeps
 * JSON trees, iterators and strings outside any Lua longjmp path. */
enum class Action { value, key, set_index, set_key };
struct Operation {
    Action action;
    const Json::Value* value = nullptr;
    std::string key;
    lua_Integer index = 0;
};
void plan(const Json::Value& value, std::vector<Operation>& operations, Budget& budget, unsigned depth) {
    budget.visit(depth);
    if (value.isString()) budget.string(value.asString());
    if (value.type() == Json::realValue && !std::isfinite(value.asDouble())) invalid("non-finite JSON number");
    operations.push_back({Action::value, &value, {}, 0});
    if (value.isArray()) {
        for (Json::ArrayIndex i = 0; i < value.size(); ++i) {
            plan(value[i], operations, budget, depth + 1);
            operations.push_back({Action::set_index, nullptr, {}, static_cast<lua_Integer>(i) + 1});
        }
    } else if (value.isObject()) {
        for (auto it = value.begin(); it != value.end(); ++it) {
            auto key = it.name();
            budget.string(key);
            operations.push_back({Action::key, nullptr, std::move(key), 0});
            plan(*it, operations, budget, depth + 1);
            operations.push_back({Action::set_key, nullptr, {}, 0});
        }
    }
}

/** Only trivial automatic variables here: allocation failures may longjmp. */
int push_plan(lua_State* state) {
    const auto* operations = static_cast<const std::vector<Operation>*>(lua_touserdata(state, 1));
    lua_settop(state, 0);
    luaL_checkstack(state, static_cast<int>(depth_limit * 2 + 8), "JSON stack limit");
    for (const auto& op : *operations) {
        switch (op.action) {
        case Action::key: lua_pushlstring(state, op.key.data(), op.key.size()); break;
        case Action::set_index: lua_rawseti(state, -2, op.index); break;
        case Action::set_key: lua_rawset(state, -3); break;
        case Action::value:
            switch (op.value->type()) {
            case Json::nullValue: lua_pushlightuserdata(state, &null_identity); break;
            case Json::booleanValue: lua_pushboolean(state, op.value->asBool()); break;
            case Json::intValue: lua_pushinteger(state, op.value->asInt64()); break;
            case Json::uintValue: lua_pushinteger(state, static_cast<lua_Integer>(op.value->asUInt64())); break;
            case Json::realValue: lua_pushnumber(state, op.value->asDouble()); break;
            case Json::stringValue: {
                const char* begin; const char* end;
                op.value->getString(&begin, &end);
                lua_pushlstring(state, begin, static_cast<std::size_t>(end - begin));
                break;
            }
            case Json::arrayValue: case Json::objectValue:
                lua_newtable(state);
                luaL_setmetatable(state, op.value->isArray() ? array_tag : object_tag);
                break;
            }
            break;
        }
    }
    return 1;
}

/** Read without coercion/metamethods or allocating Lua APIs. checkstack returns
 * failure instead of raising a Lua error; C++ exceptions unwind the JSON tree. */
Json::Value read_value(lua_State* state, int index, Budget& budget, unsigned depth,
                       std::vector<const void*>& ancestors) {
    budget.visit(depth);
    if (!lua_checkstack(state, 6)) invalid("JSON Lua stack exhausted");
    index = lua_absindex(state, index);
    switch (lua_type(state, index)) {
    case LUA_TLIGHTUSERDATA:
        if (lua_touserdata(state, index) == &null_identity) return {};
        break;
    case LUA_TBOOLEAN: return Json::Value(lua_toboolean(state, index) != 0);
    case LUA_TNUMBER:
        if (lua_isinteger(state, index)) return Json::Value(static_cast<Json::Int64>(lua_tointeger(state, index)));
        if (!std::isfinite(lua_tonumber(state, index))) invalid("non-finite Lua number");
        return Json::Value(lua_tonumber(state, index));
    case LUA_TSTRING: {
        std::size_t size;
        const char* text = lua_tolstring(state, index, &size);
        budget.string({text, size});
        return Json::Value(text, text + size);
    }
    case LUA_TTABLE: {
        const void* identity = lua_topointer(state, index);
        for (const auto* parent : ancestors) if (parent == identity) invalid("cyclic JSON table");
        ancestors.push_back(identity);
        // Registry lookups use light-userdata keys to avoid allocating strings.
        int kind = 0;
        if (lua_getmetatable(state, index)) {
            lua_rawgetp(state, LUA_REGISTRYINDEX, array_tag);
            if (lua_rawequal(state, -1, -2)) kind = 1;
            lua_pop(state, 1);
            lua_rawgetp(state, LUA_REGISTRYINDEX, object_tag);
            if (lua_rawequal(state, -1, -2)) kind = 2;
            lua_pop(state, 2);
            if (!kind) invalid("unsupported table metatable");
        }
        std::size_t count = 0, maximum = 0;
        bool string_keys = false, integer_keys = false;
        lua_pushnil(state);
        while (lua_next(state, index)) {
            ++count;
            if (count > node_limit) invalid("JSON structure limit exceeded");
            if (lua_type(state, -2) == LUA_TSTRING) {
                if (kind == 1 || integer_keys) invalid("mixed or invalid JSON table keys");
                string_keys = true;
            } else if (lua_isinteger(state, -2)) {
                if (kind == 2 || string_keys) invalid("mixed or invalid JSON table keys");
                integer_keys = true;
                const auto key = lua_tointeger(state, -2);
                if (key < 1 || key > static_cast<lua_Integer>(node_limit)) invalid("invalid JSON array index");
                maximum = std::max(maximum, static_cast<std::size_t>(key));
            } else invalid("unsupported JSON table key");
            lua_pop(state, 1);
        }
        if (integer_keys && count != maximum) invalid("JSON arrays must be dense");
        // Validate shape before allocating: a sparse large index must not grow
        // a native JSON array just to discover the holes afterwards.
        Json::Value result(kind == 1 || integer_keys ? Json::arrayValue : Json::objectValue);
        lua_pushnil(state);
        while (lua_next(state, index)) {
            if (integer_keys) {
                const auto key = static_cast<Json::ArrayIndex>(lua_tointeger(state, -2) - 1);
                result[key] = read_value(state, -1, budget, depth + 1, ancestors);
            } else {
                std::size_t size;
                const char* key = lua_tolstring(state, -2, &size);
                budget.string({key, size});
                std::string name(key, size);
                result[name] = read_value(state, -1, budget, depth + 1, ancestors);
            }
            lua_pop(state, 1);
        }
        ancestors.pop_back();
        return result;
    }
    }
    invalid("unsupported JSON value (use sipi.json.null for null)");
}

int push_encoded(lua_State* state) {
    const auto* text = static_cast<const std::string*>(lua_touserdata(state, 1));
    lua_pushlstring(state, text->data(), text->size());
    return 1;
}

/** Both directions destroy C++ owners before raising any Lua error. Protected
 * result creation also preserves Lua allocation errors without leaking owners. */
int convert(lua_State* state, bool decode) {
    char error[192]{};
    int status = LUA_OK;
    try {
        if (lua_gettop(state) != 1) invalid("JSON function expects exactly one argument");
        if (decode) {
            if (lua_type(state, 1) != LUA_TSTRING) invalid("json.decode expects a string");
            std::size_t size;
            const char* text = lua_tolstring(state, 1, &size);
            const auto value = parse_json({text, size});
            status = push_json(state, value);
        } else {
            const auto value = read_json(state, 1);
            const auto text = write_json(value);
            lua_pushcfunction(state, push_encoded);
            lua_pushlightuserdata(state, const_cast<std::string*>(&text));
            status = lua_pcall(state, 1, 1, 0);
        }
    } catch (const std::exception& exception) {
        // Do not include input fragments: JSON can contain credentials.
        std::strncpy(error, exception.what(), sizeof(error) - 1);
    } catch (...) {
        std::strcpy(error, "JSON conversion failed");
    }
    if (error[0]) return luaL_error(state, "%s", error);
    if (status != LUA_OK) return lua_error(state);
    return 1;
}
int decode(lua_State* state) { return convert(state, true); }
int encode(lua_State* state) { return convert(state, false); }

/** Explicit constructors mark fresh empty tables; decoded tables carry the same
 * locked tags. Plain empty tables encode as objects, dense sequences as arrays. */
int container(lua_State* state) {
    if (lua_gettop(state) != 0) return luaL_error(state, "JSON constructor expects no arguments");
    lua_newtable(state);
    lua_pushvalue(state, lua_upvalueindex(1));
    lua_setmetatable(state, -2);
    return 1;
}
}

Json::Value parse_json(std::string_view text) {
    if (text.size() > byte_limit) invalid("JSON input exceeds 256 KiB");
    utf8(text);
    validate_tokens(text);
    Json::CharReaderBuilder builder;
    builder["collectComments"] = false;
    builder["allowComments"] = false;
    builder["allowTrailingCommas"] = false;
    builder["strictRoot"] = false;
    builder["failIfExtra"] = true;
    builder["rejectDupKeys"] = true;
    builder["skipBom"] = false;
    builder["stackLimit"] = depth_limit;
    std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
    Json::Value value;
    std::string diagnostic;
    if (!reader->parse(text.data(), text.data() + text.size(), &value, &diagnostic))
        invalid("invalid JSON document");
    Budget budget;
    std::vector<Operation> operations;
    plan(value, operations, budget, 0); // Apply identical structural/string checks for native callers.
    return value;
}

std::string write_json(const Json::Value& value) {
    Budget budget;
    std::vector<Operation> operations;
    plan(value, operations, budget, 0);
    Json::StreamWriterBuilder builder;
    builder["indentation"] = "";
    const auto text = Json::writeString(builder, value);
    if (text.size() > byte_limit) invalid("JSON output exceeds 256 KiB");
    return text;
}

Json::Value read_json(lua_State* state, int index) {
    Budget budget;
    std::vector<const void*> ancestors;
    return read_value(state, index, budget, 0, ancestors);
}

int push_json(lua_State* state, const Json::Value& value) {
    if (!lua_checkstack(state, 2)) invalid("JSON Lua stack exhausted");
    Budget budget;
    std::vector<Operation> operations;
    plan(value, operations, budget, 0);
    lua_pushcfunction(state, push_plan);
    lua_pushlightuserdata(state, &operations);
    return lua_pcall(state, 1, 1, 0);
}

void open_json(lua_State* state) {
    lua_newtable(state); // sipi
    lua_newtable(state); // json
    lua_pushcfunction(state, decode); lua_setfield(state, -2, "decode");
    lua_pushcfunction(state, encode); lua_setfield(state, -2, "encode");
    lua_pushlightuserdata(state, &null_identity); lua_setfield(state, -2, "null");
    for (const auto* tag : {array_tag, object_tag}) {
        luaL_newmetatable(state, tag);
        lua_pushstring(state, "protected JSON container");
        lua_setfield(state, -2, "__metatable");
        lua_pushvalue(state, -1);
        lua_rawsetp(state, LUA_REGISTRYINDEX, tag);
        lua_pushcclosure(state, container, 1);
        lua_setfield(state, -2, tag == array_tag ? "array" : "object");
    }
    lua_setfield(state, -2, "json");
    lua_setglobal(state, "sipi");
}
}
