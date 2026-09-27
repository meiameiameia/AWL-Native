#include "awl/world_map_collision_registry.h"

#include <algorithm>

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

} // namespace awl
