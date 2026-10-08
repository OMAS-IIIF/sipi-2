#pragma once
struct lua_State;
namespace sipi::preflight {
class HttpSession;
/** Install sipi.http in an existing sipi table inside a protected Lua call.
 * Session is borrowed and must outlive the Lua state, including finalizers. */
void open_http(lua_State* state, HttpSession& session);
}
