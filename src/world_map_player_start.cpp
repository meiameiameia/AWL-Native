#include "awl/world_map_player_start.h"
#include <cmath>
#include <cstring>

namespace awl {
namespace {
bool valid_pose(const WorldMapPlayerScenePose& pose) {
    if (pose.scene_type < 0 || pose.scene_type > 44) return false;
    for (float value : pose.position) if (!std::isfinite(value)) return false;
    for (float value : pose.heading) if (!std::isfinite(value)) return false;
    return true;
}

uint32_t read_word(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
        (static_cast<uint32_t>(bytes[1]) << 16) |
        (static_cast<uint32_t>(bytes[2]) << 8) | bytes[3];
}

int32_t read_signed_word(const uint8_t* bytes) {
    const uint32_t word = read_word(bytes);
    int32_t value;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

float read_float(const uint8_t* bytes) {
    const uint32_t word = read_word(bytes);
    float value;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}
} // namespace

bool decode_world_map_player_start_pose(const uint8_t* bytes, size_t size,
    int32_t scene_type, WorldMapPlayerScenePose* output) {
    constexpr size_t position_offset = 0x29C78;
    constexpr size_t heading_offset = 0x29F84;
    if (!bytes || !output || size < heading_offset + 12) return false;
    WorldMapPlayerScenePose next;
    next.scene_type = scene_type;
    for (size_t i = 0; i < 3; ++i) {
        next.position[i] = read_float(bytes + position_offset + i * 4);
        next.heading[i] = read_float(bytes + heading_offset + i * 4);
    }
    if (!valid_pose(next)) return false;
    *output = next;
    return true;
}

bool make_world_map_phase_entry_pose(int32_t scene_type,
    WorldMapPlayerScenePose* output) {
    // 8023E398: BF800000 00000000 C0A66666; 80251130: +Z.
    const WorldMapPlayerScenePose next{scene_type,
        {-1.0f, 0.0f, -0x1.4cccccp+2f}, {0.0f, 0.0f, 1.0f}};
    if (!output || !valid_pose(next)) return false;
    *output = next;
    return true;
}

bool decode_world_map_player_start_inputs(
    const WorldMapPlayerStartOwners& owners, WorldMapPlayerStartInputs* output) {
    if (!output || !owners.saved || owners.saved_size < 0x2A294 ||
        !owners.scene || owners.scene_size < 0x6C ||
        !owners.guards || owners.guards_size < 0x684 ||
        !owners.secondary || owners.secondary_size < 0x3F4) return false;
    WorldMapPlayerStartInputs next;
    if (!decode_world_map_player_start_pose(owners.saved, owners.saved_size,
            read_signed_word(owners.scene + 0x68), &next.start.saved_pose)) return false;
    next.start.state_680 = read_signed_word(owners.guards + 0x680);
    next.start.state_58c = read_signed_word(owners.guards + 0x58C);
    next.start.secondary_byte_3f3 = owners.secondary[0x3F3];
    next.secondary_byte_3f2 = owners.secondary[0x3F2];
    next.saved_scene_type_2a290 = read_signed_word(owners.saved + 0x2A290);
    auto& flags = next.start.placement.static_flags;
    flags.state_299a4 = owners.saved[0x299A4] != 0;
    flags.state_299a5 = owners.saved[0x299A5] != 0;
    flags.state_299a6 = owners.saved[0x299A6] != 0;
    flags.state_299a8 = owners.saved[0x299A8] != 0;
    flags.state_299a9 = owners.saved[0x299A9] != 0;
    flags.state_299af = owners.saved[0x299AF] != 0;
    flags.secondary_3f1 = owners.secondary[0x3F1] != 0;
    flags.secondary_3f3 = next.start.secondary_byte_3f3 != 0;
    *output = next;
    return true;
}

WorldMapPlayerConstructorTailStatus plan_world_map_player_constructor_tail(
    const WorldMapPlayerConstructorTailQuery& query, WorldMapPlayerConstructorTail* output) {
    using Status = WorldMapPlayerConstructorTailStatus;
    if (!output) return Status::InvalidInput;
    WorldMapPlayerConstructorTail next;
    next.requested_state = 0x0F;
    const auto& inputs = query.inputs;
    if (inputs.start.state_680 != -1 || inputs.start.state_58c != 0) {
        *output = next;
        return Status::Ready;
    }
    if (inputs.secondary_byte_3f2 != 0) return Status::UnsupportedRestoredPose;
    // FUN_801A2A28 returns true for null or pointed word zero.
    if (query.binding_present && query.binding_word_0 != 0)
        return Status::UnsupportedBusyBinding;
    next.requested_state = 0x29;
    if (query.action_148_after_setup != 0) {
        auto saved = inputs.start.saved_pose;
        saved.scene_type = inputs.saved_scene_type_2a290;
        if (!valid_pose(saved)) return Status::InvalidInput;
        if (!query.payload_camera_byte_known) return Status::MissingCameraByteEvidence;
        next.message_prepared = true;
        next.message_target_id = query.action_148_after_setup;
        next.message = {saved.scene_type, saved.position, saved.heading,
                        query.payload_camera_byte};
    }
    *output = next;
    return Status::Ready;
}

uint32_t select_world_map_player_model_type(uint8_t byte_14c, int32_t word_20) {
    if (byte_14c != 0) return 0x24;
    const float value = static_cast<float>(word_20);
    // r13 = 8034E9C0 (80003334..38), not the stack/heap end address.
    // Thresholds 80346BA0/4: 47927BA0 (74999.25), 46C34F80 (24999.75).
    if (value >= 0x1.24f74p+16f) return 1;
    if (value >= 0x1.869fp+14f) return 0;
    return 7;
}

bool select_world_map_player_model_type_slot(
    uint32_t phase, uint32_t table_type_134, int32_t saved_27e54, uint32_t* slot) {
    if (!slot || phase >= 6 || table_type_134 > 1) return false;
    uint32_t base = 0x8029F818;
    if (table_type_134 == 1) {
        base += saved_27e54 == 3 ? 0x3A8 : saved_27e54 == 4 ? 0x3C0 : 0x3D8;
    }
    *slot = base + phase * 4;
    return true;
}

WorldMapPlayerStartStateStatus prepare_world_map_player_start_state(
    const WorldMapPlayerStartState& state, const WorldMapPlayerStartStateQuery& query,
    WorldMapPlayerStartStateStep* output) {
    using Status = WorldMapPlayerStartStateStatus;
    if (!output || &state == &output->after) return Status::InvalidInput;
    WorldMapPlayerStartStateStep next; next.after = state;
    auto stop = [&](Status status) {
        next.after = state; *output = next; return status;
    };
    if (query.requested_state != 0x0F && query.requested_state != 0x29)
        return stop(Status::UnsupportedState);
    // The remaps for states 0/1 and byte stores for state 12 are not
    // reached by either supported constructor request.
    if (query.scene_type_68 == 1) {
        if (!query.audio_byte_90) return stop(Status::RequiresAudioByte);
        if (*query.audio_byte_90 != 0) return stop(Status::RequiresAudioStop);
    }
    next.after.state_1364 = query.requested_state;
    next.model_selector = select_world_map_player_model_type(
        query.saved_subobject_byte_14c, query.saved_subobject_word_20);
    if (!select_world_map_player_model_type_slot(query.phase, state.actor_table_type_134,
            query.saved_27e54, &next.model_slot)) return stop(Status::UnsupportedModelTable);
    if (!query.model_row) return stop(Status::RequiresModelRow);
    const auto& row = *query.model_row;
    if (row.slot != next.model_slot || row.selector != next.model_selector)
        return Status::InvalidInput;
    WorldMapAnimationFeatureStep model;
    const auto status = prepare_world_map_actor_model_type(state.actor_model_type,
        row.type_0, row.type_4, query.model_observations, &model);
    next.required_timer_row = model.required_row;
    using FeatureStatus = WorldMapAnimationFeatureStatus;
    switch (status) {
    case FeatureStatus::InvalidInput: return Status::InvalidInput;
    case FeatureStatus::RequiresTable: return stop(Status::RequiresTimerTable);
    case FeatureStatus::RequiresRow: return stop(Status::RequiresTimerRow);
    case FeatureStatus::RequiresClock: return stop(Status::RequiresClock);
    case FeatureStatus::Prepared: break;
    }
    next.after.actor_model_type = model.after;
    if (query.requested_state == 0x0F) {
        next.callback_address = 0x80035884;
        next.after.phase_byte_8 = 0;
    } else {
        next.callback_address = 0x800349A4;
        if (query.secondary_byte_3f2 != 0) return stop(Status::UnsupportedRestoredPose);
        next.animation_command = WorldMapPlayerStartAnimationCommand{
            1, query.variant_138c, query.item_144, query.action_148, 4};
        return stop(Status::RequiresAnimationCommand);
    }
    // The 0F callback cannot change +1364; 80031E88..80031F5C therefore
    // follows the secondary +3F0=1 branch. The counter is not touched.
    next.after.secondary_byte_3f0 = 1;
    *output = next;
    return Status::Prepared;
}

WorldMapPlayerStartStateStatus advance_world_map_player_start_state(
    WorldMapPlayerStartState* state, const WorldMapPlayerStartStateQuery& query,
    WorldMapPlayerStartStateStep* output) {
    using Status = WorldMapPlayerStartStateStatus;
    if (!state || !output || state == &output->after) return Status::InvalidInput;
    const auto status = prepare_world_map_player_start_state(*state, query, output);
    if (status != Status::Prepared) return status;
    *state = output->after;
    return Status::Advanced;
}

WorldMapPlayerStartStatus prepare_world_map_player_start(
    const WorldMapPlayerStartQuery& query, WorldMapPlayerStart* output) {
    if (!output || !valid_pose(query.saved_pose))
        return WorldMapPlayerStartStatus::InvalidInput;
    if (query.secondary_byte_3f3 != 0)
        return WorldMapPlayerStartStatus::UnsupportedRelocation;
    if (query.placement.static_flags.secondary_3f3)
        return WorldMapPlayerStartStatus::InvalidInput;
    WorldMapPlayerStart next;
    next.pose = query.saved_pose;
    // Target/current speed and direction are the constructor's zero words;
    // facing comes from the saved heading, not the last movement direction.
    next.steering.facing_x = next.pose.heading[0];
    next.steering.facing_z = next.pose.heading[2];
    if (query.state_680 == -1 && query.state_58c == 0) {
        if (!resolve_type1_world_map_initial_placement(query.placement,
                next.pose.scene_type, next.pose.position, &next.placement))
            return WorldMapPlayerStartStatus::PlacementFailed;
        next.placement_called = true;
        next.pose.position = next.placement.position;
    }
    *output = next;
    return WorldMapPlayerStartStatus::Ready;
}
} // namespace awl
