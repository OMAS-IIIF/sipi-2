#pragma once
#include <cstdint>
#include <vector>

namespace fixture {
/** Structurally accepted RGB ICC header for byte-preservation tests only.
 * No transform tags: this is NOT a usable color-management profile. */
inline std::vector<std::uint8_t> icc_header() {
    std::vector<std::uint8_t> bytes(132);
    const auto word = [&](unsigned offset, std::uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) bytes[offset+i] = static_cast<std::uint8_t>(value >> (24-8*i));
    };
    word(0, 132); word(8, 0x04000000);
    word(12, 0x6d6e7472); // mntr
    word(16, 0x52474220); // RGB
    word(20, 0x58595a20); // XYZ
    word(36, 0x61637370); // acsp
    word(68, 0x0000f6d6); word(72, 0x00010000); word(76, 0x0000d32d); // D50
    return bytes;
}
} // namespace fixture
