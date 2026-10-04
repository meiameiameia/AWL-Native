#pragma once

#include "awl/world_map_animation_channel.h"

#include <array>
#include <memory>

namespace awl {
class WorldMapNativeModel;
class WorldMapNativeAnimationChannel;
enum class WorldMapAnimationChannelConstructionStatus { Constructed, InvalidInput, AllocationFailure };

// FUN_801A67A8 -> three FUN_8019FD64/FDE8 records, older/previous/target.
// Owns stable native record keys; original channel +14 and record +8/+C/+18
// remain unknown. This is semantic storage, not the serialized PPC/vtable ABI.
// Model blend links borrow these keys: destroy the model before this owner,
// or clear its links through a verified operation while both are still alive.
// The same borrower rule applies before replacing an existing channel owner.
// No original heap backend, frame clock/update or actor holder is constructed.
class WorldMapNativeAnimationChannel {
public:
    WorldMapNativeAnimationChannel(const WorldMapNativeAnimationChannel&) = delete;
    WorldMapNativeAnimationChannel& operator=(const WorldMapNativeAnimationChannel&) = delete;
    const WorldMapAnimationChannelState& state() const { return state_; }
    const std::vector<WorldMapAnimationPartialPlaybackRecord>& records() const { return records_; }

    [[nodiscard]] WorldMapAnimationChannelStatus setup(
        WorldMapNativeModel* model, const WorldMapActorAnimationSetup& setup,
        const WorldMapAnimationBank* bank, WorldMapAnimationPartialChannelStep* out);
    // Supported secondary caller and settings bridge; failures preserve both
    // owners. Missing banks/fields remain explicit. No descriptor acceptance.
    [[nodiscard]] WorldMapAnimationChannelStatus setup_secondary(
        WorldMapNativeModel* model, uint32_t descriptor_word_4,
        uint64_t bank_identity, const WorldMapAnimationBank* bank,
        WorldMapAnimationPartialChannelStep* out);
    [[nodiscard]] WorldMapAnimationChannelStatus apply_settings(
        WorldMapNativeModel* model, uint32_t loop, float rate,
        WorldMapAnimationPartialChannelStep* out);
private:
    WorldMapNativeAnimationChannel();
    friend WorldMapAnimationChannelConstructionStatus construct_world_map_animation_channel(
        std::unique_ptr<WorldMapNativeAnimationChannel>*);
    std::array<uint64_t, 3> identity_storage_{};
    WorldMapAnimationChannelState state_;
    std::vector<WorldMapAnimationPartialPlaybackRecord> records_;
};

// A fresh owner stages all records before replacing *out. Native cleanup and
// atomic failure publication are stronger than the original allocator path.
[[nodiscard]] WorldMapAnimationChannelConstructionStatus construct_world_map_animation_channel(
    std::unique_ptr<WorldMapNativeAnimationChannel>* out);
} // namespace awl
