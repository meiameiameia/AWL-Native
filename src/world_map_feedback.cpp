#include "awl/world_map_feedback.h"

#include <algorithm>

namespace awl {

bool WorldMapFeedbackRegistry::add_result_destination(uint64_t identity, uint32_t initial_word) {
    if (identity == 0) return false;
    for (const auto& destination : destinations_) if (destination.identity == identity) return false;
    destinations_.push_back({identity, initial_word});
    return true;
}

bool WorldMapFeedbackRegistry::result_destination_word(uint64_t identity, uint32_t* out) const {
    if (out == nullptr) return false;
    for (const auto& destination : destinations_) {
        if (destination.identity == identity) { *out = destination.word; return true; }
    }
    return false;
}

bool WorldMapFeedbackRegistry::create_request(uint64_t identity, uint32_t feedback_id,
                                             std::optional<uint64_t> result_destination) {
    if (identity == 0) return false;
    for (const auto& entry : requests_) if (entry.identity == identity) return false;
    uint32_t destination_word = 0;
    if (result_destination && !result_destination_word(*result_destination, &destination_word)) return false;
    requests_.push_back({identity, static_cast<uint16_t>(feedback_id), UINT32_MAX, result_destination});
    return true;
}

bool WorldMapFeedbackRegistry::request(uint64_t identity, WorldMapFeedbackRequest* out) const {
    if (out == nullptr) return false;
    for (const auto& entry : requests_) {
        if (entry.identity == identity) { *out = entry; return true; }
    }
    return false;
}

bool WorldMapFeedbackRegistry::link_request(uint64_t identity, uint64_t owner_identity) {
    if (owner_identity == 0) return false;
    WorldMapFeedbackRequest entry;
    if (!request(identity, &entry)) return false;
    const auto found = std::find_if(links_.begin(), links_.end(),
        [identity](const Link& link) { return link.request == identity; });
    // Reusing a link leaves capacity for its append; a new link is appended
    // without any prior mutation if allocation fails.
    if (found != links_.end()) links_.erase(found);
    links_.push_back({owner_identity, identity});
    return true;
}

bool WorldMapFeedbackRegistry::unlink_request(uint64_t identity) {
    const auto found = std::find_if(links_.begin(), links_.end(),
        [identity](const Link& link) { return link.request == identity; });
    if (found == links_.end()) return false;
    links_.erase(found);
    return true;
}

void WorldMapFeedbackRegistry::clear_owner(uint64_t owner_identity) {
    links_.erase(std::remove_if(links_.begin(), links_.end(),
        [owner_identity](const Link& link) { return link.owner == owner_identity; }), links_.end());
}

size_t WorldMapFeedbackRegistry::size(uint64_t owner_identity) const {
    return static_cast<size_t>(std::count_if(links_.begin(), links_.end(),
        [owner_identity](const Link& link) { return link.owner == owner_identity; }));
}

bool WorldMapFeedbackRegistry::find_request(uint64_t owner_identity, uint32_t feedback_id,
                                           std::optional<uint64_t> after, uint64_t* out) const {
    if (owner_identity == 0 || out == nullptr) return false;
    bool passed_cursor = !after;
    if (after) {
        const auto cursor = std::find_if(links_.begin(), links_.end(),
            [owner_identity, after](const Link& link) { return link.owner == owner_identity && link.request == *after; });
        if (cursor == links_.end()) return false;
    }
    for (const auto& link : links_) {
        if (link.owner != owner_identity) continue;
        if (!passed_cursor) { passed_cursor = link.request == *after; continue; }
        WorldMapFeedbackRequest entry;
        if (request(link.request, &entry) && entry.id_14 == static_cast<uint16_t>(feedback_id)) {
            *out = entry.identity;
            return true;
        }
    }
    return false;
}

void WorldMapFeedbackRegistry::write_result(WorldMapFeedbackRequest& entry, uint32_t word) {
    entry.result_18 = word;
    if (entry.result_destination) {
        for (auto& destination : destinations_) {
            if (destination.identity == *entry.result_destination) { destination.word = word; break; }
        }
    }
}

bool WorldMapFeedbackRegistry::destroy_request(uint64_t identity) {
    const auto found = std::find_if(requests_.begin(), requests_.end(),
        [identity](const WorldMapFeedbackRequest& entry) { return entry.identity == identity; });
    if (found == requests_.end()) return false;
    write_result(*found, UINT32_MAX);
    (void)unlink_request(identity);
    requests_.erase(found);
    return true;
}

WorldMapFeedbackStatus WorldMapFeedbackRegistry::start_request(uint64_t identity,
    const std::array<uint32_t, 3>& arguments, std::optional<uint32_t> backend_result, WorldMapFeedbackStep* out) {
    using Status = WorldMapFeedbackStatus;
    if (out == nullptr) return Status::InvalidInput;
    const auto found = std::find_if(requests_.begin(), requests_.end(),
        [identity](const WorldMapFeedbackRequest& entry) { return entry.identity == identity; });
    if (found == requests_.end()) return Status::InvalidInput;
    WorldMapFeedbackStep step;
    step.request_identity = identity;
    step.feedback_id = found->id_14;
    step.playback_handle = found->result_18;
    if (found->result_18 != UINT32_MAX) { *out = step; return Status::Unchanged; }
    step.operation = WorldMapFeedbackOperation::Start;
    step.arguments = arguments;
    if (!backend_result) { *out = step; return Status::RequiresPlayback; }
    write_result(*found, *backend_result);
    step.return_word = *backend_result != UINT32_MAX ? 1u : 0u;
    *out = step;
    return Status::Applied;
}

WorldMapFeedbackStatus WorldMapFeedbackRegistry::stop_request(uint64_t identity,
    std::optional<uint32_t> backend_result, WorldMapFeedbackStep* out) {
    using Status = WorldMapFeedbackStatus;
    if (out == nullptr) return Status::InvalidInput;
    const auto found = std::find_if(requests_.begin(), requests_.end(),
        [identity](const WorldMapFeedbackRequest& entry) { return entry.identity == identity; });
    if (found == requests_.end()) return Status::InvalidInput;
    WorldMapFeedbackStep step;
    step.request_identity = identity;
    step.feedback_id = found->id_14;
    step.playback_handle = found->result_18;
    if (found->result_18 == UINT32_MAX) { step.return_word = 1; *out = step; return Status::Unchanged; }
    step.operation = WorldMapFeedbackOperation::Stop;
    if (!backend_result) { *out = step; return Status::RequiresPlayback; }
    step.return_word = *backend_result;
    if ((*backend_result & 0xffu) != 0) write_result(*found, UINT32_MAX);
    *out = step;
    return Status::Applied;
}
} // namespace awl
