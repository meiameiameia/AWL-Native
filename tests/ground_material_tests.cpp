#include "awl/ground_material.h"

#include <cstdint>
#include <iostream>

#define ASSERT_TRUE(condition)                                                   \
    do {                                                                         \
        if (!(condition)) {                                                       \
            std::cerr << "Assertion failed: " << #condition << " at "           \
                      << __FILE__ << ':' << __LINE__ << std::endl;                \
            return false;                                                        \
        }                                                                        \
    } while (0)

#define ASSERT_FALSE(condition) ASSERT_TRUE(!(condition))

namespace {

bool test_verified_ground_variant_table() {
    constexpr uint8_t expected[4][10] = {
        {1, 2, 3, 3, 3, 3, 3, 3, 4, 5},
        {6, 7, 8, 8, 8, 8, 8, 8, 9, 10},
        {11, 12, 13, 13, 13, 13, 13, 13, 14, 15},
        {16, 17, 18, 18, 18, 18, 18, 18, 19, 20},
    };

    for (uint32_t period = 0; period < 4; ++period) {
        for (uint32_t step = 0; step < 10; ++step) {
            awl::GroundTextureBinding binding;
            ASSERT_TRUE(awl::build_ground_texture_binding(period, step, &binding));
            ASSERT_TRUE(binding.variant == expected[period][step]);
            ASSERT_TRUE(binding.alias == "@ground.tpl");
            ASSERT_TRUE(binding.logical_path ==
                        "/files/image.jimen-L_s" +
                            std::to_string(expected[period][step]) + ".tpl");
        }
    }
    return true;
}

bool test_invalid_ground_state_is_rejected() {
    awl::GroundTextureBinding binding;
    binding.alias = "stale";
    ASSERT_FALSE(awl::build_ground_texture_binding(4, 0, &binding));
    ASSERT_TRUE(binding.alias.empty());
    ASSERT_FALSE(awl::build_ground_texture_binding(0, 10, &binding));
    ASSERT_FALSE(awl::build_ground_texture_binding(0, 0, nullptr));
    return true;
}

} // namespace

int main() {
    if (!test_verified_ground_variant_table() || !test_invalid_ground_state_is_rejected()) {
        return 1;
    }
    std::cout << "[TEST RESULT] Ground material binding tests passed." << std::endl;
    return 0;
}
