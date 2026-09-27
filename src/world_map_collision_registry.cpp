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
           list == WorldMapCollisionList::Later;
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

bool WorldMapCollisionRegistry::register_object(
    WorldMapCollisionList list,
    const WorldMapRegisteredCollisionObject& object) {
    if (!valid_list(list) || object.collision.identity == 0) {
        return false;
    }
    // FUN_8001FA90 unlinks an already linked node before inserting it at the
    // requested list's front. Identity represents that node in this view.
    const WorldMapRegisteredCollisionObject replacement = object;
    auto& target = list == WorldMapCollisionList::First ? first_ : later_;
    target.reserve(target.size() + 1);
    (void)erase_identity(first_, object.collision.identity);
    (void)erase_identity(later_, object.collision.identity);
    target.insert(target.begin(), replacement);
    return true;
}

bool WorldMapCollisionRegistry::unregister_object(uint64_t identity) {
    if (identity == 0) {
        return false;
    }
    return erase_identity(first_, identity) || erase_identity(later_, identity);
}

void WorldMapCollisionRegistry::clear(WorldMapCollisionList list) {
    if (valid_list(list)) {
        (list == WorldMapCollisionList::First ? first_ : later_).clear();
    }
}

size_t WorldMapCollisionRegistry::size(WorldMapCollisionList list) const {
    if (!valid_list(list)) {
        return 0;
    }
    return (list == WorldMapCollisionList::First ? first_ : later_).size();
}

WorldMapCollisionSnapshot WorldMapCollisionRegistry::snapshot() const {
    WorldMapCollisionSnapshot result;
    result.first_resolver.reserve(first_.size());
    result.first_directional.reserve(first_.size());
    result.later_resolver.reserve(later_.size());
    for (const WorldMapRegisteredCollisionObject& object : first_) {
        result.first_resolver.push_back(object.collision);
        result.first_directional.push_back(directional_view(object));
    }
    for (const WorldMapRegisteredCollisionObject& object : later_) {
        result.later_resolver.push_back(object.collision);
    }
    return result;
}

} // namespace awl
