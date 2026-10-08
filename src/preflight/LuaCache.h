#pragma once
#include "SharedCache.h"
struct lua_State;
namespace sipi::preflight {
/** Per-evaluation guard, outliving lua_close. Shared values live in SharedCache.
 * No cache operations during initialization/finalizers. Nested loaders are banned
 * to prevent dependency cycles between worker threads. Waits share a 5s deadline. */
struct CacheSession {
    SharedCache& cache;
    bool enabled = false;
    bool loading = false;
    unsigned operations = 0;
    std::optional<SharedCache::Clock::time_point> deadline;
};
/** Register sipi.cache in an outer protected Lua call; borrows the session. */
void open_cache(lua_State* state, CacheSession& session);
}
