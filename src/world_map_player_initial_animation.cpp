#include "awl/world_map_player_initial_animation.h"
#include "awl/world_map_player_start_animation.h"
#include "awl/disc_identity.h"
#include "awl/filesystem.h"
#include "world_map_dol_view.h"
#include <fstream>
#include <cmath>
#include <cstring>
#include <new>
#include <utility>

namespace awl {
namespace {
using Status = WorldMapPlayerInitialAnimationStatus;
bool reset_type(uint32_t value, uint32_t minimum) { return value >= minimum && value <= INT32_MAX; }
} // namespace
bool WorldMapPlayerInitialAnimationInputs::decode(const uint8_t* data, size_t size) {
    detail::DolView view;
    if (!view.initialize(data, size)) return false;
    const auto* jump = view.read(0x8029E4B8, 4);
    const auto* row = view.read(0x80249230, 8);
    const auto* item = view.read(0x80282DF8, 1);
    if (!jump || detail::dol_word(jump) != 0x800289A8 || !row || !item) return false;
    const auto choice = classify_world_map_player_start_animation(0, 0, *item);
    const auto* index = row + choice * 2;
    descriptor_index_ = static_cast<uint16_t>((uint16_t(index[0]) << 8) | index[1]);
    const auto* slot = view.read(0x8029BECC + uint32_t(descriptor_index_) * 4, 4);
    if (!slot) return false;
    const auto address = detail::dol_word(slot);
    if (address == 0 || (address & 3) != 0) return false;
    const auto* descriptor = view.read(address, 12);
    if (!descriptor) return false;
    descriptor_ = {address, detail::dol_word(descriptor), detail::dol_word(descriptor + 4), detail::dol_word(descriptor + 8)};
    for (uint32_t phase = 0; phase < types_.size(); ++phase) {
        const auto* pointer = view.read(0x8029F818 + phase * 4, 4);
        if (!pointer) return false;
        const auto type_address = detail::dol_word(pointer);
        if (type_address == 0 || (type_address & 3) != 0) return false;
        const auto* types = view.read(type_address, 8);
        if (!types) return false;
        types_[phase] = {detail::dol_word(types), detail::dol_word(types + 4)};
        // Each actual slot-zero phase writes both timers. Retaining an
        // unwritten timer requires a future partial-feature consumer.
        if (!reset_type(types_[phase][0], 0x3A) || !reset_type(types_[phase][1], 0x24)) return false;
        const auto* catalog_index = view.read(0x8024A6E0 + phase, 1);
        if (!catalog_index) return false;
        const auto* scale = view.read(0x80249A6C + uint32_t(*catalog_index) * 0x18 + 4, 4);
        if (!scale) return false;
        const auto word = detail::dol_word(scale);
        std::memcpy(&scales_[phase], &word, sizeof(word));
        if (!std::isfinite(scales_[phase])) return false;
    }
    return true;
}
WorldMapPlayerInitialAnimationStep WorldMapPlayerInitialAnimationInputs::select(const WorldMapPlayerInitialAnimationQuery& query) const {
    if (query.model_slot != 0) return {Status::UnsupportedModelSlot};
    if (query.alternate != UINT32_MAX) return {Status::UnsupportedAlternate};
    if (query.phase >= types_.size()) return {Status::RequiresPhase};
    return {Status::Selected, WorldMapPlayerInitialAnimationSelection{query.phase, descriptor_index_, descriptor_,
        types_[query.phase][0], types_[query.phase][1], 0, 0, scales_[query.phase]}};
}
Status prepare_world_map_player_initial_root_pose(const WorldMapAnimationPose& before, float scale,
    WorldMapAnimationPose* out) {
    if (!out || !std::isfinite(scale)) return Status::InvalidInput;
    auto next = before;
    next[0] = (before[0] & 0x00FFFFFFu) | 0x01000000u;
    uint32_t word; std::memcpy(&word, &scale, sizeof(word));
    next[1] = next[2] = next[3] = word;
    *out = next; return Status::Prepared;
}
Status decode_world_map_player_initial_animation_inputs(const uint8_t* data, size_t size,
    std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>* out) {
    if (!data || !out) return Status::InvalidInput;
    try {
        auto next = std::shared_ptr<WorldMapPlayerInitialAnimationInputs>(new WorldMapPlayerInitialAnimationInputs);
        if (!next->decode(data, size)) return Status::UnsupportedLayout;
        *out = std::move(next); return Status::Decoded;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
Status load_world_map_player_initial_animation_inputs(std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>* out) {
    if (!out) return Status::InvalidInput;
    if (*out && (*out)->target_verified()) return Status::Loaded;
    try {
        char path[1024];
        if (!filesystem_resolve_path("/sys/main.dol", path, sizeof(path))) return Status::ReadFailure;
        std::ifstream input(path, std::ios::binary | std::ios::ate);
        if (!input) return Status::ReadFailure;
        const auto size = input.tellg();
        if (size < 0 || size > static_cast<std::streamoff>(detail::max_dol_size)) return Status::ReadFailure;
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        input.seekg(0);
        if (!input || (size != 0 && !input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) ||
            input.peek() != std::char_traits<char>::eof() || input.bad()) return Status::ReadFailure;
        Sha1 sha;
        if (!sha1_bytes(bytes.data(), bytes.size(), &sha)) return Status::ReadFailure;
        if (sha != kTargetDolSha1) return Status::WrongDol;
        auto next = std::shared_ptr<WorldMapPlayerInitialAnimationInputs>(new WorldMapPlayerInitialAnimationInputs);
        if (!next->decode(bytes.data(), bytes.size())) return Status::UnsupportedLayout;
        next->target_verified_ = true; *out = std::move(next); return Status::Loaded;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
WorldMapPlayerInitialAnimationStep prepare_world_map_player_initial_animation(
    const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>& inputs,
    const WorldMapPlayerInitialAnimationQuery& query, const WorldMapAnimationInitializerObservations& observations) {
    if (!inputs) return {Status::RequiresInputs};
    auto result = inputs->select(query);
    if (result.status != Status::Selected) return result;
    // F39C's own stores leave both timers unwritten. Their private placeholders
    // are not read by F3E8: decode requires BOTH selected types to reset them
    // from the initial 13/F types. Never expose these placeholders on a stop.
    WorldMapAnimationFeature38 fresh{0x13, 0xF, UINT32_MAX, 0};
    WorldMapAnimationFeatureStep feature;
    const auto status = prepare_world_map_actor_model_type(fresh, result.selection->type_0, result.selection->type_4,
        observations, &feature);
    result.required_row = feature.required_row;
    switch (status) {
    case WorldMapAnimationFeatureStatus::Prepared:
        result.status = Status::Prepared; result.feature = feature.after; break;
    case WorldMapAnimationFeatureStatus::RequiresTable: result.status = Status::RequiresTimerTable; break;
    case WorldMapAnimationFeatureStatus::RequiresRow: result.status = Status::RequiresTimerRow; break;
    case WorldMapAnimationFeatureStatus::RequiresClock: result.status = Status::RequiresClock; break;
    default: result.status = Status::InvalidInput; break;
    }
    return result;
}
} // namespace awl
