#pragma once

#include <cstdint>
#include <string>

namespace awl {

struct GroundTextureBinding {
    std::string alias;
    std::string logical_path;
    uint32_t variant = 0;
};

// Verified from GYWE41 main.dol SHA1 1ccfd9df... at FUN_8001BF3C.
// The semantic names of the two game-state axes are not yet proven, so the
// interface intentionally calls them period and step.
bool build_ground_texture_binding(uint32_t period_index, uint32_t step_index,
                                  GroundTextureBinding* out_binding);

} // namespace awl
