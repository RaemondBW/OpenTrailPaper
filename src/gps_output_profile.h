#pragma once
// CASIC: GGA/RMC and fix quality at 1 Hz; satellite detail every five fixes.
// Preserve all three constellations and receiver positioning frequency.
namespace gps_output_profile {
constexpr const char* lean = "PCAS03,1,0,1,5,1,0,0,0,0,0,,,0,0";
inline unsigned checksum(const char* body) {
    unsigned result = 0;
    while (*body) result ^= static_cast<unsigned char>(*body++);
    return result;
}
}
