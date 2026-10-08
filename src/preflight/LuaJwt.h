#pragma once
struct lua_State;
namespace sipi::preflight {
/** Install generic JWT helpers in the existing sipi table, under a protected Lua
 * call. A shared per-state counter bounds sign/verify work to 32 operations. */
void open_jwt(lua_State* state);
}
