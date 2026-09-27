#include "awl/world_map_collision_registry.h"

#include "awl/world_map_collision_records.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace awl {

namespace {

bool erase_identity(std::vector<WorldMapRegisteredCollisionObject>& objects,
                    uint64_t identity) {
    const auto found = std::find_if(
        objects.begin(), objects.end(),
        [identity](const WorldMapRegisteredCollisionObject& object) {
            return object.collision.identity == identity;
        });
    if (found == objects.end()) {
        return false;
    }
    objects.erase(found);
    return true;
}

bool valid_list(WorldMapCollisionList list) {
    return list == WorldMapCollisionList::First ||
           list == WorldMapCollisionList::Later ||
           list == WorldMapCollisionList::Third;
}

WorldMapContactObject directional_view(
    const WorldMapRegisteredCollisionObject& object) {
    const CollisionDynamicPassObject& collision = object.collision;
    WorldMapContactObject view;
    view.enabled = collision.enabled;
    view.category = collision.category;
    view.collision_flags = collision.collision_flags;
    view.data = collision.data;
    view.size = collision.size;
    view.contact_query.world_to_object = collision.world_to_object;
    view.contact_query.object_to_world = collision.object_to_world;
    view.contact_query.object_center_local = collision.center_local;
    view.contact_query.object_radius = collision.radius;
    view.world_position = object.world_position;
    view.heading_axis = object.heading_axis;
    view.metadata = object.metadata;
    return view;
}

} // namespace

bool world_map_collision_flags_for_mode(int32_t mode, uint32_t* flags) {
    if (flags != nullptr) {
        *flags = 0;
    }
    if (flags == nullptr || mode < 0 || mode > 4) {
        return false;
    }
    // FUN_801527A8 selects the mode word, then ORs common bit 0x40.
    constexpr std::array<uint32_t, 5> mode_bits{
        0x27u, 0x94u, 0x12fu, 0x12fu, 0x10fu};
    *flags = mode_bits[static_cast<size_t>(mode)] | 0x40u;
    return true;
}

WorldMapFirstActorCircleSpec world_map_first_actor_circle_spec(
    int32_t actor_id) {
    // FUN_80152218: the +0x24 radius values are from r2=0x80351E40.
    if (actor_id == 0x15 || actor_id == 0x23) {
        return {1, 0.6f};
    }
    if (actor_id == 0x25) {
        return {1, 0.3f};
    }
    if (actor_id >= 0x27 && actor_id <= 0x30) {
        return {2, actor_id == 0x2f ? 0.9f : 0.3f};
    }
    return {1, 0.5f};
}

bool register_world_map_scene_first_collision_objects(
    const WorldMapSceneFirstCollisionObjects& scene,
    WorldMapCollisionRegistry* registry) {
    if (registry == nullptr || scene.category1_actor.collision.identity == 0) {
        return false;
    }
    const uint64_t category1_identity = scene.category1_actor.collision.identity;
    const auto valid_optional = [category1_identity](
                                    const std::optional<WorldMapRegisteredCollisionObject>&
                                        object) {
        return !object || (object->collision.identity != 0 &&
                           object->collision.identity != category1_identity);
    };
    if (!valid_optional(scene.conditional_a) ||
        !valid_optional(scene.conditional_b) ||
        (scene.conditional_a && scene.conditional_b &&
         scene.conditional_a->collision.identity ==
             scene.conditional_b->collision.identity)) {
        return false;
    }
    for (float component : scene.category1_actor.world_position) {
        if (!std::isfinite(component)) {
            return false;
        }
    }

    WorldMapCollisionRegistry staged = *registry;
    if (scene.conditional_a &&
        !staged.register_object(WorldMapCollisionList::First,
                                *scene.conditional_a)) {
        return false;
    }
    if (scene.conditional_b &&
        !staged.register_object(WorldMapCollisionList::First,
                                *scene.conditional_b)) {
        return false;
    }
    WorldMapRegisteredCollisionObject category1 = scene.category1_actor;
    category1.collision.enabled = true;
    category1.collision.category = 1;
    category1.collision.collision_flags = 1u;
    category1.collision.data = nullptr;
    category1.collision.size = 0;
    category1.collision.radius =
        world_map_first_actor_circle_spec(scene.category1_actor_id).radius;
    category1.collision.center_world = category1.world_position;
    if (!staged.register_object(WorldMapCollisionList::First, category1)) {
        return false;
    }
    *registry = std::move(staged);
    return true;
}

bool WorldMapCollisionRegistry::register_object(
    WorldMapCollisionList list,
    const WorldMapRegisteredCollisionObject& object) {
    if (!valid_list(list) || object.collision.identity == 0) {
        return false;
    }
    // FUN_8001FA90 unlinks an already linked node before inserting it at the
    // requested list's front. Identity represents that node in this view.
    const WorldMapRegisteredCollisionObject replacement = object;
    auto& target = lists_[static_cast<size_t>(list)];
    target.reserve(target.size() + 1);
    for (auto& objects : lists_) {
        (void)erase_identity(objects, object.collision.identity);
    }
    target.insert(target.begin(), replacement);
    return true;
}

bool WorldMapCollisionRegistry::unregister_object(uint64_t identity) {
    if (identity == 0) {
        return false;
    }
    for (auto& objects : lists_) {
        if (erase_identity(objects, identity)) {
            return true;
        }
    }
    return false;
}

void WorldMapCollisionRegistry::clear(WorldMapCollisionList list) {
    if (valid_list(list)) {
        lists_[static_cast<size_t>(list)].clear();
    }
}

size_t WorldMapCollisionRegistry::size(WorldMapCollisionList list) const {
    if (!valid_list(list)) {
        return 0;
    }
    return lists_[static_cast<size_t>(list)].size();
}

WorldMapCollisionSnapshot WorldMapCollisionRegistry::snapshot() const {
    WorldMapCollisionSnapshot result;
    const auto& first = lists_[static_cast<size_t>(WorldMapCollisionList::First)];
    const auto& later = lists_[static_cast<size_t>(WorldMapCollisionList::Later)];
    const auto& third = lists_[static_cast<size_t>(WorldMapCollisionList::Third)];
    result.first_resolver.reserve(first.size());
    result.first_directional.reserve(first.size());
    result.later_resolver.reserve(later.size());
    result.third_resolver.reserve(third.size());
    result.third_objects.reserve(third.size());
    for (const WorldMapRegisteredCollisionObject& object : first) {
        result.first_resolver.push_back(object.collision);
        result.first_directional.push_back(directional_view(object));
    }
    for (const WorldMapRegisteredCollisionObject& object : later) {
        result.later_resolver.push_back(object.collision);
    }
    for (const WorldMapRegisteredCollisionObject& object : third) {
        result.third_resolver.push_back(object.collision);
        result.third_objects.push_back(object);
    }
    return result;
}

std::array<bool, 25> world_map_fixed_collision_activation(
    const WorldMapFixedCollisionState& state) {
    std::array<bool, 25> enabled{};
    for (size_t i = 0; i < 8; ++i) {
        enabled[i] = state.direct_enabled[i] != 0;
        enabled[8 + i] = state.selected_variant[i] == 0;
        enabled[16 + i] = state.selected_variant[i] == 1;
    }
    enabled[24] = state.state_299ae != 0;
    return enabled;
}

std::array<WorldMapFixedCollisionRecordKey, 25>
world_map_fixed_collision_record_keys() {
    // 0x8023EBC8 is {3,7,2,6,1,5,0,4}; FUN_80021440 adds 15 for
    // direct objects, then 24 + 8*variant for the next two groups.
    constexpr std::array<uint32_t, 8> order{3, 7, 2, 6, 1, 5, 0, 4};
    std::array<WorldMapFixedCollisionRecordKey, 25> keys{};
    for (size_t i = 0; i < order.size(); ++i) {
        keys[i] = {order[i] + 15, 7};
        keys[8 + i] = {order[i] + 24, 7};
        keys[16 + i] = {order[i] + 32, 7};
    }
    keys[24] = {23, 13};
    return keys;
}

bool bind_world_map_fixed_collision_records(
    const WorldMapCollisionRecordPools& pools,
    std::array<WorldMapRegisteredCollisionObject, 25>* objects) {
    if (objects == nullptr) {
        return false;
    }
    auto staged = *objects;
    const auto keys = world_map_fixed_collision_record_keys();
    for (size_t i = 0; i < keys.size(); ++i) {
        WorldMapCollisionRecordView view;
        if (!pools.lookup(1, keys[i].archive_index, &view) ||
            view.data == nullptr || view.analysis == nullptr) {
            return false;
        }
        auto& collision = staged[i].collision;
        collision.category = keys[i].category;
        collision.data = view.data;
        collision.size = view.size;
        collision.center_local = view.center_local;
    }
    *objects = staged;
    return true;
}

bool register_world_map_fixed_collision_objects(
    const WorldMapFixedCollisionState& state,
    const std::array<WorldMapRegisteredCollisionObject, 25>& objects,
    WorldMapCollisionRegistry* registry) {
    if (registry == nullptr) {
        return false;
    }
    for (size_t i = 0; i < objects.size(); ++i) {
        const uint64_t identity = objects[i].collision.identity;
        if (identity == 0) {
            return false;
        }
        for (size_t j = 0; j < i; ++j) {
            if (objects[j].collision.identity == identity) {
                return false;
            }
        }
    }

    const auto enabled = world_map_fixed_collision_activation(state);
    WorldMapCollisionRegistry staged = *registry;
    for (size_t i = 0; i < objects.size(); ++i) {
        WorldMapRegisteredCollisionObject object = objects[i];
        object.collision.enabled = enabled[i];
        object.collision.collision_flags |= 2u;
        if (!staged.register_object(WorldMapCollisionList::Later, object)) {
            return false;
        }
    }
    *registry = std::move(staged);
    return true;
}

} // namespace awl
