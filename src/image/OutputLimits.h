#pragma once
#include <cstdint>

namespace sipi {
/** Public output policy, independent of source dimensions and authorization.
 * max_height=0 is the internal "inherit max_width" default; configuration rejects
 * explicit zero. Hard ceilings fit the current buffered renderer, not IIIF itself.
 * Limits apply before rotation; the rotated canvas is separately budget-checked.
 */
struct OutputLimits {
    static constexpr std::uint32_t axis_ceiling = 32768;
    static constexpr std::uint64_t area_ceiling = 8 * 1024 * 1024;
    std::uint32_t max_width = 8192;
    std::uint32_t max_height = 0;
    std::uint64_t max_area = area_ceiling;
};
} // namespace sipi
