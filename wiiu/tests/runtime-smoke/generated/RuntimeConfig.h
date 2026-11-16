// Stand-in for the translator's generated/RuntimeConfig.h, used only by the Wii U
// runtime smoke test (no game code). The values are Mario Kart Wii PAL's Small Data
// Area bases from projects/mkwii/recomp.yml.
#pragma once
#include <cstdint>
namespace RuntimeConfig {
constexpr uint32_t SDA1_BASE = 0x8038CC00u;  // memory.sda_base
constexpr uint32_t SDA2_BASE = 0x8038EFA0u;  // memory.sda2_base
} // namespace RuntimeConfig
