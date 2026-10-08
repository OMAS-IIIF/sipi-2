#pragma once

#include <string>
#include <string_view>

struct lua_State;
namespace Json { class Value; }

namespace sipi::preflight {
/** Install per-state sipi.json helpers. Call inside a protected Lua boundary.
 * No shared mutable state; all values belong to the calling Lua interpreter. */
void open_json(lua_State* state);

/** Shared strict JSON conversion for native Lua modules. Native owners unwind
 * through C++ exceptions; parse/write preserve the JSON module's limits. */
Json::Value parse_json(std::string_view text);
std::string write_json(const Json::Value& value);
/** Raw non-coercing Lua reads; does not raise Lua errors. May throw C++ errors. */
Json::Value read_json(lua_State* state, int index);
/** Push under an inner protected call. Returns LUA_OK or leaves a Lua error;
 * never propagate that error until the caller has destroyed native owners. */
int push_json(lua_State* state, const Json::Value& value);
}
