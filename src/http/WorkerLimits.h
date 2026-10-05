#pragma once
#include <algorithm>
#include <thread>

namespace sipi::http {
/** Conservative CPU-derived concurrency: at least one, at most four by default.
 * The cap limits multiplication of per-request codec/raster memory; it is not a
 * process memory quota. Explicit operator settings may use 1..32 workers.
 * Accept a CPU count for deterministic tests; zero means detection unavailable. */
inline unsigned default_worker_count(unsigned cores = std::thread::hardware_concurrency()) {
    return std::clamp(cores, 1u, 4u);
}
inline constexpr unsigned maximum_worker_count = 32;
}
