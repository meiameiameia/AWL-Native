#include "awl/world_map_animation_channel_owner.h"
#include "awl/world_map_model_initialization.h"

#include <new>
#include <utility>

namespace awl {
WorldMapNativeAnimationChannel::WorldMapNativeAnimationChannel() {
    state_.blend_14.reset();
    records_.reserve(3);
    // FD64 allocates older, then previous, then target. FDE8 writes only
    // position +0, rate +1, null clip and null link in each playback record.
    for (const auto& storage : identity_storage_) {
        const auto key = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&storage));
        WorldMapAnimationPartialPlayback playback;
        playback.rate_4 = 1.0f;
        records_.push_back({key, playback});
    }
    state_.older_10 = records_[0].identity;
    state_.previous_c = records_[1].identity;
    state_.target_8 = records_[2].identity;
}

WorldMapAnimationChannelConstructionStatus construct_world_map_animation_channel(
    std::unique_ptr<WorldMapNativeAnimationChannel>* out) {
    using Status = WorldMapAnimationChannelConstructionStatus;
    if (out == nullptr) return Status::InvalidInput;
    try {
        auto owner = std::unique_ptr<WorldMapNativeAnimationChannel>(new WorldMapNativeAnimationChannel);
        *out = std::move(owner);
        return Status::Constructed;
    } catch (const std::bad_alloc&) {
        return Status::AllocationFailure;
    }
}

WorldMapAnimationChannelStatus WorldMapNativeAnimationChannel::setup(
    WorldMapNativeModel* model, const WorldMapActorAnimationSetup& setup,
    const WorldMapAnimationBank* bank, WorldMapAnimationPartialChannelStep* out) {
    return advance_world_map_native_animation_channel(model, &state_, &records_, setup, bank, out);
}
WorldMapAnimationChannelStatus WorldMapNativeAnimationChannel::setup_secondary(
    WorldMapNativeModel* model, uint32_t descriptor_word,
    uint64_t bank_identity, const WorldMapAnimationBank* bank,
    WorldMapAnimationPartialChannelStep* out) {
    return advance_world_map_native_secondary_channel(
        model, &state_, &records_, descriptor_word, bank_identity, bank, out);
}
WorldMapAnimationChannelStatus WorldMapNativeAnimationChannel::apply_settings(
    WorldMapNativeModel* model, uint32_t loop, float rate,
    WorldMapAnimationPartialChannelStep* out) {
    return apply_world_map_native_animation_channel_settings(model, &state_, &records_, loop, rate, out);
}
} // namespace awl
