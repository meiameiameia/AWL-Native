#include "awl/world_map_scene_index.h"

#include <algorithm>
#include <cmath>

namespace awl {

namespace {

uint8_t bin_coordinate(float coordinate,
                       float first,
                       float second,
                       float third) {
    if (coordinate < first) {
        return 0;
    }
    if (coordinate < second) {
        return 1;
    }
    if (coordinate < third) {
        return 2;
    }
    return 3;
}

} // namespace

bool plan_world_map_scene_position_update(
    int32_t scene_type,
    int32_t previous_bucket,
    const std::array<float, 3>& resolved_position,
    WorldMapScenePositionUpdate* update) {
    if (update != nullptr) {
        *update = {};
    }
    if (update == nullptr || scene_type < 0 || scene_type > 44 ||
        previous_bucket < -1 || previous_bucket > 58 ||
        !std::isfinite(resolved_position[0]) ||
        !std::isfinite(resolved_position[1]) ||
        !std::isfinite(resolved_position[2])) {
        return false;
    }

    WorldMapScenePositionUpdate candidate;
    candidate.position = resolved_position;
    candidate.previous_bucket = previous_bucket;
    if (scene_type == 1 || scene_type == 2) {
        // FUN_8001153C uses >= at each threshold. The third X constant has
        // DOL float bits 0x4318F9E9; the 4x4 table is 1..16.
        const uint8_t x_bin = bin_coordinate(
            resolved_position[0], 54.0f, 99.0f, 152.97621f);
        const uint8_t z_bin = bin_coordinate(
            resolved_position[2], 64.0f, 130.0f, 194.0f);
        candidate.next_bucket = static_cast<uint8_t>(1 + x_bin * 4 + z_bin);
    } else if (scene_type >= 3) {
        // The verified 42-pair table at 0x8023DFE8 maps 3..44 to 17..58.
        candidate.next_bucket = static_cast<uint8_t>(scene_type + 14);
    }
    candidate.relink_required = candidate.next_bucket != previous_bucket;
    *update = candidate;
    return true;
}

bool WorldMapSceneBucketRegistry::register_object(
    uint64_t identity,
    int32_t scene_type,
    const std::array<float, 3>& position) {
    WorldMapScenePositionUpdate update;
    if (identity == 0 || !plan_world_map_scene_position_update(
                             scene_type, -1, position, &update)) {
        return false;
    }
    for (const auto& bucket : buckets_) {
        if (std::any_of(bucket.begin(), bucket.end(),
                        [identity](const WorldMapSceneObject& object) {
                            return object.identity == identity;
                        })) {
            return false;
        }
    }
    // FUN_800110D4 initializes the bucket key to -1; FUN_800109C0
    // registers that node before the first FUN_800107A4 position write.
    auto& initial_bucket = buckets_[0];
    initial_bucket.insert(initial_bucket.begin(),
                          WorldMapSceneObject{identity, scene_type, position,
                                              -1});
    return true;
}

bool WorldMapSceneBucketRegistry::update_position(
    uint64_t identity,
    const std::array<float, 3>& position,
    WorldMapScenePositionUpdate* update) {
    return update_object(identity, false, 0, position, update);
}

bool WorldMapSceneBucketRegistry::update_type_and_position(
    uint64_t identity,
    int32_t scene_type,
    const std::array<float, 3>& position,
    WorldMapScenePositionUpdate* update) {
    return update_object(identity, true, scene_type, position, update);
}

bool WorldMapSceneBucketRegistry::update_object(
    uint64_t identity,
    bool replace_type,
    int32_t scene_type,
    const std::array<float, 3>& position,
    WorldMapScenePositionUpdate* update) {
    if (update != nullptr) {
        *update = {};
    }
    if (identity == 0 || update == nullptr) {
        return false;
    }
    for (auto& bucket : buckets_) {
        const auto found = std::find_if(
            bucket.begin(), bucket.end(),
            [identity](const WorldMapSceneObject& object) {
                return object.identity == identity;
            });
        if (found == bucket.end()) {
            continue;
        }
        const int32_t next_type = replace_type ? scene_type : found->scene_type;
        WorldMapScenePositionUpdate planned;
        if (!plan_world_map_scene_position_update(
                next_type, found->bucket, position, &planned)) {
            return false;
        }
        if (!planned.relink_required) {
            found->scene_type = next_type;
            found->position = position;
        } else {
            auto& next = buckets_[static_cast<size_t>(planned.next_bucket) + 1];
            next.reserve(next.size() + 1);
            WorldMapSceneObject moved = *found;
            moved.scene_type = next_type;
            moved.position = position;
            moved.bucket = planned.next_bucket;
            bucket.erase(found);
            next.insert(next.begin(), moved);
        }
        *update = planned;
        return true;
    }
    return false;
}

bool apply_world_map_player_scene_message_1f(
    uint64_t scene_identity,
    const WorldMapPlayerSceneMessage1F& message,
    WorldMapSceneBucketRegistry* registry,
    WorldMapPlayerScenePose* pose,
    WorldMapScenePositionUpdate* update) {
    if (update != nullptr) {
        *update = {};
    }
    if (registry == nullptr || pose == nullptr || update == nullptr ||
        !std::isfinite(message.heading[0]) ||
        !std::isfinite(message.heading[1]) ||
        !std::isfinite(message.heading[2])) {
        return false;
    }
    if (!registry->update_type_and_position(scene_identity, message.scene_type,
                                            message.position, update)) {
        return false;
    }
    *pose = {message.scene_type, message.position, message.heading};
    return true;
}

bool apply_world_map_player_fixed_scene_message_1f(
    WorldMapSceneBucketRegistry* registry,
    WorldMapPlayerScenePose* pose,
    WorldMapScenePositionUpdate* update) {
    // FUN_80013DC8 copies the scene type and camera byte from its constant
    // payload, then overwrites XYZ and heading from two verified DOL tables.
    // The camera byte is zero; camera effects are outside this helper.
    constexpr WorldMapPlayerSceneMessage1F message{
        3, {-1.0f, 0.0f, -5.2f}, {0.0f, 0.0f, 1.0f}};
    return apply_world_map_player_scene_message_1f(
        1, message, registry, pose, update);
}

bool apply_world_map_scene_mode_request(
    WorldMapSceneModeRequestState* state,
    int32_t requested_mode,
    uint8_t scene_byte) {
    if (state == nullptr) {
        return false;
    }
    state->scene_byte_78 = scene_byte;
    state->mode_58 = requested_mode;
    if (requested_mode == 4 || requested_mode == 13) {
        state->state_64 = 1;
    }
    // FUN_800126E0 marks every mode request. FUN_80012740 classifies
    // 4, 6, and 14 for the additional prior-mode flag.
    state->global_flag_59af = 1;
    const auto classified = [](int32_t mode) {
        return mode == 4 || mode == 6 || mode == 14;
    };
    if (classified(requested_mode) && classified(state->previous_mode_5c)) {
        state->global_flag_59b0 = 1;
    }
    return true;
}

bool reset_world_map_player_sequence_step(
    WorldMapPlayerFixedTransitionStepState* state) {
    if (state == nullptr ||
        (state->sequence_step_4574 != 2 && state->sequence_step_4574 != 5)) {
        return false;
    }
    state->sequence_step_4574 = 0;
    return true;
}

bool apply_world_map_player_fixed_transition_step(
    WorldMapPlayerFixedTransitionStepState* state,
    WorldMapSceneBucketRegistry* registry,
    WorldMapPlayerScenePose* pose,
    WorldMapScenePositionUpdate* update) {
    if (update != nullptr) {
        *update = {};
    }
    if (state == nullptr || update == nullptr || state->state_680 != -1 ||
        state->state_58c != 0 || state->sequence_step_4574 != 1) {
        return false;
    }
    if (!apply_world_map_player_fixed_scene_message_1f(
            registry, pose, update)) {
        return false;
    }
    // The native subset commits these only after the supported scene-pose
    // update succeeds, so a missing player node cannot half-advance it.
    state->scene_byte_79 = 0;
    state->sequence_step_4574 = 2;
    return true;
}

bool WorldMapSceneBucketRegistry::unregister_object(uint64_t identity) {
    if (identity == 0) {
        return false;
    }
    for (auto& bucket : buckets_) {
        const auto found = std::find_if(
            bucket.begin(), bucket.end(),
            [identity](const WorldMapSceneObject& object) {
                return object.identity == identity;
            });
        if (found != bucket.end()) {
            bucket.erase(found);
            return true;
        }
    }
    return false;
}

void WorldMapSceneBucketRegistry::clear() {
    for (auto& bucket : buckets_) {
        bucket.clear();
    }
}

size_t WorldMapSceneBucketRegistry::size(int32_t bucket) const {
    return bucket >= -1 && bucket <= 58
               ? buckets_[static_cast<size_t>(bucket + 1)].size()
               : 0;
}

std::vector<WorldMapSceneObject> WorldMapSceneBucketRegistry::snapshot(
    int32_t bucket) const {
    return bucket >= -1 && bucket <= 58
               ? buckets_[static_cast<size_t>(bucket + 1)]
               : std::vector<WorldMapSceneObject>{};
}

} // namespace awl
