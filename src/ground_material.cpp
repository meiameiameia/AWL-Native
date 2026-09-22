#include "awl/ground_material.h"

namespace awl {
namespace {

// Bytes at 0x802994F0, converted to the one-based variant used by
// FUN_8001BF3C when formatting "image.jimen-L_s%d.tpl".
constexpr uint8_t kGroundVariantTable[4][10] = {
    {1, 2, 3, 3, 3, 3, 3, 3, 4, 5},
    {6, 7, 8, 8, 8, 8, 8, 8, 9, 10},
    {11, 12, 13, 13, 13, 13, 13, 13, 14, 15},
    {16, 17, 18, 18, 18, 18, 18, 18, 19, 20},
};

} // namespace

bool build_ground_texture_binding(uint32_t period_index, uint32_t step_index,
                                  GroundTextureBinding* out_binding) {
    if (!out_binding) {
        return false;
    }
    *out_binding = {};
    if (period_index >= 4 || step_index >= 10) {
        return false;
    }

    out_binding->variant = kGroundVariantTable[period_index][step_index];
    out_binding->alias = "@ground.tpl";
    out_binding->logical_path =
        "/files/image.jimen-L_s" + std::to_string(out_binding->variant) + ".tpl";
    return true;
}

} // namespace awl
