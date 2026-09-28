#include "awl/world_map_camera.h"

#include "awl/collision_asset.h"
#include "awl/filesystem.h"
#include "awl/platform.h"

#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>

namespace awl {

namespace {

struct CameraRegion {
    float min_x;
    float max_x;
    float min_z;
    float max_z;
    float yaw;
    std::array<float, 3> first_corner;
    std::array<float, 3> second_corner;
};

// FUN_8001D0E8's ordered, inclusive rectangles (r2-0x7E64..-0x7E3C)
// and FUN_80030E18's profile rows at 0x8029FE10, stride 0x1C.
constexpr CameraRegion kRegions[] = {
    {171.0f, 180.0f, 111.0f, 118.0f, 0.0f,
     {173.5f, 0.0f, 120.5f}, {177.6f, 100.0f, 122.3f}},
    {192.0f, 199.0f, 111.0f, 120.0f, 0.0f,
     {194.5f, 0.0f, 119.6f}, {196.6f, 100.0f, 123.6f}},
    {216.0f, 229.0f, 112.0f, 130.0f, 4.71238899f,
     {211.1f, 0.0f, 115.2f}, {219.6f, 100.0f, 125.8f}},
};

bool finite_vector(const std::array<float, 3>& value) {
    return std::isfinite(value[0]) && std::isfinite(value[1]) &&
           std::isfinite(value[2]);
}

bool finite_state(const WorldMapCameraFollowupState& value) {
    return finite_vector(value.position) && std::isfinite(value.field_18) &&
           std::isfinite(value.yaw) && finite_vector(value.bounds_min) &&
           finite_vector(value.bounds_max);
}

std::array<float, 3> rotate_x_then_y(const std::array<float, 3>& value,
                                     float pitch, float yaw) {
    const float sin_pitch = std::sin(pitch);
    const float cos_pitch = std::cos(pitch);
    const float sin_yaw = std::sin(yaw);
    const float cos_yaw = std::cos(yaw);
    const float x_after_x = value[0];
    const float y_after_x = cos_pitch * value[1] - sin_pitch * value[2];
    const float z_after_x = sin_pitch * value[1] + cos_pitch * value[2];
    return {cos_yaw * x_after_x + sin_yaw * z_after_x,
            y_after_x,
            -sin_yaw * x_after_x + cos_yaw * z_after_x};
}

std::array<float, 3> cross_product(const std::array<float, 3>& a,
                                   const std::array<float, 3>& b) {
    return {a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0]};
}

bool normalize_vector(std::array<float, 3>* value) {
    if (value == nullptr || !finite_vector(*value)) {
        return false;
    }
    const double length = std::sqrt(
        static_cast<double>((*value)[0]) * (*value)[0] +
        static_cast<double>((*value)[1]) * (*value)[1] +
        static_cast<double>((*value)[2]) * (*value)[2]);
    if (!std::isfinite(length) || length == 0.0) {
        return false;
    }
    for (float& component : *value) {
        component = static_cast<float>(component / length);
    }
    return finite_vector(*value);
}

float negative_dot(const std::array<float, 3>& a,
                   const std::array<float, 3>& b) {
    return -(a[0] * b[0] + a[1] * b[1] + a[2] * b[2]);
}

bool build_view_matrix(const std::array<float, 3>& target,
                       const std::array<float, 3>& second_point,
                       const std::array<float, 3>& up,
                       std::array<float, 12>* output) {
    if (output == nullptr || !finite_vector(target) ||
        !finite_vector(second_point) || !finite_vector(up)) {
        return false;
    }
    std::array<float, 3> forward{};
    for (size_t axis = 0; axis < 3; ++axis) {
        forward[axis] = target[axis] - second_point[axis];
    }
    if (!normalize_vector(&forward)) {
        return false;
    }
    std::array<float, 3> right = cross_product(up, forward);
    if (!normalize_vector(&right)) {
        return false;
    }
    const std::array<float, 3> corrected_up =
        cross_product(forward, right);
    if (!finite_vector(corrected_up)) {
        return false;
    }
    const std::array<std::array<float, 3>, 3> rows{
        right, corrected_up, forward};
    std::array<float, 12> matrix{};
    for (size_t row = 0; row < 3; ++row) {
        for (size_t axis = 0; axis < 3; ++axis) {
            matrix[row * 4 + axis] = rows[row][axis];
        }
        matrix[row * 4 + 3] = negative_dot(target, rows[row]);
    }
    for (float component : matrix) {
        if (!std::isfinite(component)) {
            return false;
        }
    }
    *output = matrix;
    return true;
}

bool calculate_plane(float yaw, const std::array<float, 3>& target,
                     WorldMapCameraPlane* output) {
    if (output == nullptr || !std::isfinite(yaw) ||
        !finite_vector(target)) {
        return false;
    }
    WorldMapCameraPlane next;
    next.normal = {-std::sin(yaw), 0.0f, -std::cos(yaw)};
    next.constant = negative_dot(target, next.normal);
    if (!finite_vector(next.normal) || !std::isfinite(next.constant)) {
        return false;
    }
    *output = next;
    return true;
}

struct CameraCollisionContext {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

bool sample_camera_collision_height(const std::array<float, 3>& target,
                                    float* height, void* context) {
    if (height == nullptr || context == nullptr) {
        return false;
    }
    const auto* collision = static_cast<const CameraCollisionContext*>(context);
    CollisionHeightSample sample;
    if (!sample_type1_collision_height(collision->data, collision->size,
                                       target[0], target[2], &sample)) {
        return false;
    }
    *height = sample.height;
    return true;
}

} // namespace

bool plan_world_map_camera_followup(
    const WorldMapCameraFollowupState& previous, int32_t category,
    const std::array<float, 3>& resolved_position,
    uint8_t global_byte_3f1, int8_t pad_byte_8e,
    WorldMapCameraFollowup* output) {
    if (output == nullptr || !finite_state(previous) ||
        !finite_vector(resolved_position)) {
        return false;
    }
    WorldMapCameraFollowup next;
    next.state = previous;
    next.state.position = resolved_position;
    if (category == 1) {
        for (int32_t index = 0; index < 3; ++index) {
            const CameraRegion& region = kRegions[index];
            if (resolved_position[0] < region.min_x ||
                resolved_position[0] > region.max_x ||
                resolved_position[2] < region.min_z ||
                resolved_position[2] > region.max_z) {
                continue;
            }
            next.region_index = index;
            next.state.field_18 = -1.012291f; // r2-0x7CA8
            next.state.yaw = region.yaw;
            next.state.bounds_min = region.first_corner;
            next.state.bounds_max = region.second_corner;
            for (size_t axis = 0; axis < 3; ++axis) {
                if (next.state.bounds_min[axis] > next.state.bounds_max[axis]) {
                    std::swap(next.state.bounds_min[axis],
                              next.state.bounds_max[axis]);
                }
            }
            next.state.flag_99 = true;
            next.state.flag_98 = false;
            *output = next;
            return true;
        }
        if (global_byte_3f1 == 0) {
            next.yaw_adjustment_called = true;
            if (previous.flag_98) {
                // FUN_80085FD0: signed PAD byte at 0x8034158E, scaled by
                // r2-0x77D8 (1/256); square and restore its sign against
                // r2-0x77FC (zero) before adding to camera +0x1C.
                const float scaled =
                    static_cast<float>(pad_byte_8e) * 0.00390625f;
                const float square = scaled * scaled;
                const float delta = scaled < 0.0f ? -square : square;
                next.state.yaw = previous.yaw + delta;
                if (!std::isfinite(next.state.yaw)) {
                    return false;
                }
                next.yaw_adjustment_written = true;
            }
        }
        next.state.field_18 = -0.14835298f; // r2-0x7CA4
        next.state.flag_99 = false;
        next.state.flag_98 = true;
    }
    *output = next;
    return true;
}

bool calculate_world_map_camera_target(
    const WorldMapCameraTargetQuery& query, WorldMapCameraTarget* output) {
    if (output == nullptr || !finite_state(query.camera) ||
        !finite_vector(query.origin_offset_0c) ||
        !std::isfinite(query.distance_30) ||
        !std::isfinite(query.pitch_offset_8c) ||
        !std::isfinite(query.yaw_offset_90)) {
        return false;
    }
    const float pitch = query.camera.field_18 + query.pitch_offset_8c;
    const float yaw = query.camera.yaw + query.yaw_offset_90;
    if (!std::isfinite(pitch) || !std::isfinite(yaw)) {
        return false;
    }

    // FUN_8017B908 rotates (0, 0, +0x30) by the X matrix at pitch and then
    // the Y matrix at yaw, before adding camera +0x00 and +0x0C.
    const float horizontal = query.distance_30 * std::cos(pitch);
    const std::array<float, 3> rotated{
        horizontal * std::sin(yaw),
        -query.distance_30 * std::sin(pitch),
        horizontal * std::cos(yaw)};
    WorldMapCameraTarget next;
    for (size_t axis = 0; axis < 3; ++axis) {
        next.raw[axis] = query.camera.position[axis] +
                         query.origin_offset_0c[axis] + rotated[axis];
    }
    if (!finite_vector(next.raw)) {
        return false;
    }
    next.bounded = next.raw;
    if (query.camera.flag_99) {
        // FUN_8017B9C4 compares lower and then upper bound on each axis.
        for (size_t axis = 0; axis < 3; ++axis) {
            if (next.bounded[axis] < query.camera.bounds_min[axis]) {
                next.bounded[axis] = query.camera.bounds_min[axis];
            }
            if (next.bounded[axis] > query.camera.bounds_max[axis]) {
                next.bounded[axis] = query.camera.bounds_max[axis];
            }
        }
        next.clamped = next.bounded != next.raw;
    }
    *output = next;
    return true;
}

bool calculate_world_map_camera_view(
    const WorldMapCameraViewQuery& query, WorldMapCameraView* output) {
    if (output == nullptr || !finite_vector(query.up_vector_24) ||
        !std::isfinite(query.pitch_offset_44) ||
        !std::isfinite(query.yaw_offset_48)) {
        return false;
    }
    WorldMapCameraView next;
    if (!calculate_world_map_camera_target(query.target, &next.target)) {
        return false;
    }
    const float pitch = query.target.camera.field_18 +
                        query.target.pitch_offset_8c;
    const float yaw = query.target.camera.yaw + query.target.yaw_offset_90;
    const float point_pitch = pitch + query.pitch_offset_44;
    const float point_yaw = yaw + query.yaw_offset_48;
    const float reverse_distance = -2.0f * query.target.distance_30;
    if (!std::isfinite(point_pitch) || !std::isfinite(point_yaw) ||
        !std::isfinite(reverse_distance)) {
        return false;
    }
    // FUN_8017BA50: the second point is based on the already bounded target.
    const std::array<float, 3> reverse = rotate_x_then_y(
        {0.0f, 0.0f, reverse_distance}, point_pitch, point_yaw);
    next.rotated_up = rotate_x_then_y(query.up_vector_24, pitch, yaw);
    for (size_t axis = 0; axis < 3; ++axis) {
        next.second_point[axis] = next.target.bounded[axis] + reverse[axis];
    }
    if (!finite_vector(next.second_point) ||
        !finite_vector(next.rotated_up)) {
        return false;
    }

    // FUN_801B8454 writes the 3x4 view matrix from target, second point,
    // and rotated up vector.
    if (!build_view_matrix(next.target.bounded, next.second_point,
                           next.rotated_up, &next.matrix_50)) {
        return false;
    }
    *output = next;
    return true;
}

bool make_world_map_camera_initial_profile(
    WorldMapCameraInitialProfile* output) {
    if (output == nullptr) {
        return false;
    }
    WorldMapCameraInitialProfile next;
    // FUN_8017B5F0 clears the view offsets and sets +0x98=1, +0x99=0.
    // FUN_8008558C then copies the world-map row at 0x802AB940 through
    // FUN_8017B6D4, overwriting the base camera's first 0x44 bytes.
    next.view_query.target.camera.flag_98 = true;
    next.view_query.target.camera.field_18 = -0.14835298f;
    next.view_query.target.camera.yaw = 4.71238899f;
    next.view_query.target.origin_offset_0c = {0.0f, 1.5f, 0.0f};
    next.view_query.target.distance_30 = 12.0f;
    next.view_query.up_vector_24 = {0.0f, 1.0f, 0.0f};
    next.field_34 = 1.33333337f;
    next.field_38 = 30.2000008f;
    next.field_3c = 0.300000012f;
    next.field_40 = 1024.0f;
    // FUN_80085D18 writes mode zero; the initialized mode-zero entry at
    // 0x802AB984 dispatches to the no-op thunk at 0x80085C5C.
    if (!calculate_world_map_camera_view(next.view_query,
                                          &next.initial_view)) {
        return false;
    }
    *output = next;
    return true;
}

bool calculate_world_map_camera_post_update(
    const WorldMapCameraViewQuery& query,
    WorldMapCameraHeightSampler sample_height, void* sample_context,
    WorldMapCameraPostUpdate* output) {
    if (output == nullptr || sample_height == nullptr) {
        return false;
    }
    WorldMapCameraPostUpdate next;
    if (!calculate_world_map_camera_view(query, &next.first_view) ||
        !calculate_plane(query.target.camera.yaw,
                         next.first_view.target.bounded,
                         &next.first_plane)) {
        return false;
    }
    float first_height = 0.0f;
    if (!sample_height(next.first_view.target.bounded, &first_height,
                       sample_context) || !std::isfinite(first_height)) {
        return false;
    }

    // FUN_80085998: terrain height at the first target determines a
    // temporary pitch offset. The Y offset appears on both sides of the
    // subtraction in the DOL and is retained here in the same order.
    const float y_offset = query.target.origin_offset_0c[1];
    const float height_with_offset = first_height + y_offset;
    const float camera_with_offset = query.target.camera.position[1] +
                                     y_offset;
    const float vertical = height_with_offset - camera_with_offset;
    const float angle = static_cast<float>(
        std::atan2(static_cast<double>(vertical),
                   static_cast<double>(query.target.distance_30)));
    next.temporary_pitch_offset_8c = -angle;
    if (!std::isfinite(next.temporary_pitch_offset_8c)) {
        return false;
    }
    WorldMapCameraViewQuery pitched_query = query;
    pitched_query.target.pitch_offset_8c =
        next.temporary_pitch_offset_8c;
    pitched_query.target.yaw_offset_90 = 0.0f; // DOL vector at 0x8024F72C
    if (!calculate_world_map_camera_view(pitched_query,
                                          &next.pitched_view) ||
        !calculate_plane(query.target.camera.yaw,
                         next.pitched_view.target.bounded,
                         &next.final_plane)) {
        return false;
    }
    next.final_target = next.pitched_view.target.bounded;
    next.final_matrix_50 = next.pitched_view.matrix_50;

    // FUN_80085998 clears +0x8C/+0x90/+0x94 before the second height query.
    float second_height = 0.0f;
    if (!sample_height(next.pitched_view.target.bounded, &second_height,
                       sample_context) || !std::isfinite(second_height)) {
        return false;
    }
    if (next.final_target[1] < second_height) {
        next.final_target[1] = second_height;
        std::array<float, 3> second_point{};
        for (size_t axis = 0; axis < 3; ++axis) {
            second_point[axis] = query.target.camera.position[axis] +
                                 query.target.origin_offset_0c[axis];
        }
        const std::array<float, 3> reset_up = rotate_x_then_y(
            query.up_vector_24, query.target.camera.field_18,
            query.target.camera.yaw);
        // FUN_8017B89C replaces the target and rebuilds the matrix using
        // the sum of camera vectors at +0x00 and +0x0C as the second point.
        if (!build_view_matrix(next.final_target, second_point, reset_up,
                               &next.final_matrix_50)) {
            return false;
        }
        next.terrain_clamped = true;
    }
    *output = next;
    return true;
}

bool calculate_world_map_player_camera_placement(
    const WorldMapPlayerCameraPlacementQuery& query,
    WorldMapCameraHeightSampler sample_height, void* sample_context,
    WorldMapPlayerCameraPlacement* output) {
    if (output == nullptr || sample_height == nullptr ||
        !finite_vector(query.player_position)) {
        return false;
    }
    WorldMapPlayerCameraPlacement next;
    if (!plan_world_map_camera_followup(
            query.initial_camera.target.camera, query.collision_category,
            query.player_position, query.global_byte_3f1, query.pad_byte_8e,
            &next.first_followup)) {
        return false;
    }
    WorldMapCameraViewQuery current = query.initial_camera;
    current.target.camera = next.first_followup.state;
    if (!calculate_world_map_camera_post_update(
            current, sample_height, sample_context, &next.first_update)) {
        return false;
    }
    // FUN_80085998 clears camera +0x8C/+0x90 before the constructor can
    // repeat this pair of updates.
    current.target.pitch_offset_8c = 0.0f;
    current.target.yaw_offset_90 = 0.0f;
    next.final_camera = current;
    next.final_update = next.first_update;
    if (next.first_followup.region_index < 0) {
        // FUN_8002FDF8 0x80030264..0x800302A0 chooses scene-mode yaw,
        // writes it only under the current +0x98 flag, then copies the
        // player position regardless of that flag.
        float selected_yaw = query.fallback_yaw;
        if (query.scene_mode == 4 || query.scene_mode == 6) {
            if (!std::isfinite(query.heading_x) ||
                !std::isfinite(query.heading_z)) {
                return false;
            }
            selected_yaw = static_cast<float>(std::atan2(
                static_cast<double>(query.heading_x),
                static_cast<double>(query.heading_z)));
        }
        if (!std::isfinite(selected_yaw)) {
            return false;
        }
        next.second_update_called = true;
        next.second_yaw_written = current.target.camera.flag_98;
        if (next.second_yaw_written) {
            current.target.camera.yaw = selected_yaw;
        }
        current.target.camera.position = query.player_position;
        if (!calculate_world_map_camera_post_update(
                current, sample_height, sample_context,
                &next.final_update)) {
            return false;
        }
        next.final_camera = current;
    }
    *output = next;
    return true;
}

bool calculate_world_map_player_message_camera_update(
    const WorldMapPlayerCameraMessageQuery& query,
    WorldMapCameraHeightSampler sample_height, void* sample_context,
    WorldMapPlayerCameraMessageResult* output) {
    if (output == nullptr) {
        return false;
    }
    WorldMapPlayerCameraMessageResult next;
    next.final_camera = query.previous_camera;
    if (query.message.camera_update_requested == 0) {
        *output = next;
        return true;
    }
    if (sample_height == nullptr ||
        !plan_world_map_camera_followup(
            query.previous_camera.target.camera, query.collision_category,
            query.message.position, query.global_byte_3f1,
            query.pad_byte_8e, &next.followup)) {
        return false;
    }
    next.final_camera.target.camera = next.followup.state;
    if (!calculate_world_map_camera_post_update(
            next.final_camera, sample_height, sample_context,
            &next.update)) {
        return false;
    }
    // FUN_80085998 clears these temporary offsets after the two queries.
    next.final_camera.target.pitch_offset_8c = 0.0f;
    next.final_camera.target.yaw_offset_90 = 0.0f;
    next.update_called = true;
    *output = next;
    return true;
}

bool calculate_world_map_camera_post_update_from_collision(
    const WorldMapCameraViewQuery& query, const uint8_t* camera_col,
    size_t camera_col_size, WorldMapCameraPostUpdate* output) {
    CollisionTreeAnalysis analysis;
    if (output == nullptr ||
        !analyze_type1_collision_asset(camera_col, camera_col_size,
                                       &analysis) ||
        analysis.header_byte_6 != 0) {
        return false;
    }
    CameraCollisionContext context{camera_col, camera_col_size};
    return calculate_world_map_camera_post_update(
        query, sample_camera_collision_height, &context, output);
}

bool WorldMapCameraCollisionAsset::load() {
    clear();
    void* raw = nullptr;
    size_t size = 0;
    if (!filesystem_read_entire_file(kWorldMapCameraCollisionPath, &raw,
                                     &size)) {
        return false;
    }
    std::unique_ptr<void, decltype(&filesystem_free_file_data)> owned(
        raw, &filesystem_free_file_data);
    const auto* first = static_cast<const uint8_t*>(raw);
    std::vector<uint8_t> next(first, first + size);
    CollisionTreeAnalysis analysis;
    if (!analyze_type1_collision_asset(next.data(), next.size(), &analysis) ||
        analysis.header_byte_6 != 0) {
        AWL_LOG_ERROR("Unsupported camera collision asset: %s",
                      kWorldMapCameraCollisionPath);
        return false;
    }
    bytes_.swap(next);
    return true;
}

void WorldMapCameraCollisionAsset::clear() {
    bytes_.clear();
}

bool WorldMapCameraCollisionAsset::calculate_post_update(
    const WorldMapCameraViewQuery& query,
    WorldMapCameraPostUpdate* output) const {
    return !bytes_.empty() &&
           calculate_world_map_camera_post_update_from_collision(
               query, bytes_.data(), bytes_.size(), output);
}

bool WorldMapCameraCollisionAsset::calculate_player_placement(
    const WorldMapPlayerCameraPlacementQuery& query,
    WorldMapPlayerCameraPlacement* output) const {
    if (bytes_.empty()) {
        return false;
    }
    CameraCollisionContext context{bytes_.data(), bytes_.size()};
    return calculate_world_map_player_camera_placement(
        query, sample_camera_collision_height, &context, output);
}

bool WorldMapCameraCollisionAsset::calculate_player_message_update(
    const WorldMapPlayerCameraMessageQuery& query,
    WorldMapPlayerCameraMessageResult* output) const {
    if (query.message.camera_update_requested == 0) {
        return calculate_world_map_player_message_camera_update(
            query, nullptr, nullptr, output);
    }
    if (bytes_.empty()) {
        return false;
    }
    CameraCollisionContext context{bytes_.data(), bytes_.size()};
    return calculate_world_map_player_message_camera_update(
        query, sample_camera_collision_height, &context, output);
}

} // namespace awl
