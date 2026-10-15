#include "awl/world_map_player_update.h"
#include <algorithm>
#include <cmath>
#include <cfenv>
#include <new>
#include <utility>

namespace awl {
WorldMapPlayerState1AnimationResult prepare_world_map_player_state1_animation(
    std::optional<uint32_t> control,const std::optional<WorldMapPlayerState1AnimationProfile>& profile,
    std::optional<float> intensity,std::optional<float> speed,const std::optional<WorldMapPlayerUpdateGuards>& guards,
    std::optional<uint32_t> item,std::optional<uint32_t> action) noexcept {
    using S=WorldMapPlayerState1AnimationStatus;WorldMapPlayerState1AnimationResult result;
    const auto stop=[&](S status){result.status=status;return result;};
    if(!control)return stop(S::RequiresControl);if(*control&0x800u)return stop(S::SkippedByControl);
    if(!profile)return stop(S::RequiresProfile);if(!intensity)return stop(S::RequiresIntensity);
    if(!std::isfinite(profile->zero) || !std::isfinite(profile->unit) || !std::isfinite(profile->multiplier) ||
        !std::isfinite(profile->divisor) || profile->divisor==0 || !std::isfinite(*intensity) || std::fegetround()!=FE_TONEAREST)return stop(S::UnsupportedNumerics);
    uint32_t selector=*intensity==profile->unit?3u:2u;
    if(*intensity==profile->zero){
        selector=0;result.final_speed=profile->unit;
        if(!guards)return stop(S::RequiresGuards);
        bool skip=guards->word_680!=-1;
        if(!skip){if(!guards->word_58c)return stop(S::RequiresGuardWord58c);skip=*guards->word_58c!=0;}
        if(skip)return stop(S::Prepared);
    }
    if(!speed)return stop(S::RequiresSpeed);
    const float multiplied=profile->multiplier * *speed;
    const float rate=multiplied/profile->divisor;
    if(!std::isfinite(rate))return stop(S::UnsupportedNumerics);
    if(!action)return stop(S::RequiresAction);if(!item)return stop(S::RequiresItem);
    result.command=WorldMapPlayerStartAnimationCommand{selector,0x35,*item,*action,4};result.command_speed=rate;
    if(!result.final_speed)result.final_speed=rate;
    return stop(S::Prepared);
}
WorldMapPlayerState1InitializerResult prepare_world_map_player_state1_initializer(
    std::optional<int32_t> action,std::optional<uint32_t> mask,bool owner) noexcept {
    using S=WorldMapPlayerState1InitializerStatus;
    if(!action)return {S::RequiresAction};
    uint32_t selected=0;
    if(*action!=0 && *action!=2){if(!mask)return {S::RequiresMask};selected=*mask;}
    return {owner?S::Prepared:S::RequiresItemOwner,selected};
}
WorldMapPlayerState0SecondaryResult prepare_world_map_player_state0_secondary(
    const std::optional<WorldMapPlayerUpdateGuards>& guards,std::optional<uint8_t> completed,
    std::optional<uint8_t> flag,std::optional<float> intensity) noexcept {
    using S=WorldMapPlayerState0SecondaryStatus;
    if(!guards)return {S::RequiresGuards};
    if(guards->word_680!=-1)return {S::SkippedByGuard};
    if(!guards->word_58c)return {S::RequiresGuardWord58c};
    if(*guards->word_58c!=0)return {S::SkippedByGuard};
    if(!completed)return {S::RequiresHolderCompleted};
    if(*completed)return {S::TransitionRequired,1};
    if(!flag)return {S::RequiresHolderFlag};
    if(*flag)return {S::RetainedState};
    if(!intensity)return {S::RequiresMovement};
    if(!std::isfinite(*intensity))return {S::UnsupportedNumerics};
    return *intensity==0.0f?WorldMapPlayerState0SecondaryResult{S::RequiresBody}:
        WorldMapPlayerState0SecondaryResult{S::TransitionRequired,1};
}
WorldMapPlayerFollowupStatus prepare_world_map_player_state29_tertiary(
    const std::optional<WorldMapPlayerUpdateGuards>& guards) noexcept {
    using S=WorldMapPlayerFollowupStatus;
    if (!guards) return S::RequiresGuards;
    if (guards->word_680!=-1) return S::SkippedByGuard;
    if (!guards->word_58c) return S::RequiresGuardWord58c;
    return *guards->word_58c==0?S::RetainedState:S::SkippedByGuard;
}
WorldMapPlayerState29SecondaryResult prepare_world_map_player_state29_secondary(
    const WorldMapPlayerState29SecondaryInput& input) noexcept {
    using S=WorldMapPlayerFollowupStatus;
    const auto guard=prepare_world_map_player_state29_tertiary(input.guards);
    if (guard!=S::RetainedState) return {guard,{}};
    if (!input.mode_word_60) return {S::RequiresModeWord,{}};
    if (*input.mode_word_60==1) return {S::RetainedState,{}};
    if (!input.scene_mode_60) return {S::RequiresSceneMode,{}};
    const auto transition=[](int32_t state) { return WorldMapPlayerState29SecondaryResult{S::TransitionRequired,state}; };
    switch (*input.scene_mode_60) {
    case 0x1d:
        if (!input.saved_byte_14d35) return {S::RequiresSavedByte,{}};
        return transition(*input.saved_byte_14d35?0xe1:0);
    case 0x23: return transition(0xce);
    case 0x25:
        if (!input.secondary_word_c) return {S::RequiresSecondaryWordC,{}};
        return transition(*input.secondary_word_c==0?0xcc:0);
    case 0x26: return transition(0xd9);
    case 0x28:
        if (!input.secondary_word_20) return {S::RequiresSecondaryWord20,{}};
        return transition(*input.secondary_word_20==0?0xce:0);
    default:
        if (!input.secondary_byte_3f2) return {S::RequiresSecondaryByte,{}};
        if (*input.secondary_byte_3f2) return transition(0x12);
        if (!input.item_present) return {S::RequiresItemOwner,{}};
        if (!*input.item_present) return transition(0);
        if (!input.item_type_6) return {S::RequiresItemType,{}};
        return transition(*input.item_type_6==7?0xb0:0);
    }
}

WorldMapPlayerUpdateStatus prepare_world_map_player_state29_update_entry(
    const WorldMapPlayerUpdateEntry& entry) {
    return prepare_world_map_player_normal_update_entry(entry);
}
WorldMapPlayerUpdateStatus prepare_world_map_player_normal_update_entry(
    const WorldMapPlayerUpdateEntry& entry) {
    using S = WorldMapPlayerUpdateStatus;
    if (!entry.control_word) return S::RequiresControlWord;
    if (!entry.guards) return S::RequiresGuardOwner;
    if (entry.guards->word_680 != -1) return S::RequiresGuardTransition;
    if (!entry.guards->word_58c) return S::RequiresGuardWord58c;
    if (*entry.guards->word_58c != 0) return S::RequiresGuardTransition;
    if (!entry.scene_child_word_4) return S::RequiresSceneWord;
    if (*entry.scene_child_word_4 == 1) return S::RequiresSceneTransition;
    return S::Prepared;
}

WorldMapPlayerUpdateStatus prepare_world_map_player_state29_update_tail(
    const WorldMapPlayerUpdateOwners& owners,const WorldMapPlayerScenePose& pose,
    uint64_t player_node_identity,WorldMapPlayerUpdateOwners* after) {
    return prepare_world_map_player_normal_update_tail(owners,pose,player_node_identity,after);
}
WorldMapPlayerUpdateStatus prepare_world_map_player_normal_update_tail(
    const WorldMapPlayerUpdateOwners& owners, const WorldMapPlayerScenePose& pose,
    uint64_t player_node_identity, WorldMapPlayerUpdateOwners* after) {
    using S = WorldMapPlayerUpdateStatus;
    if (!after) return S::InvalidInput;
    for (float value : pose.position) if (!std::isfinite(value)) return S::InvalidInput;
    for (float value : pose.heading) if (!std::isfinite(value)) return S::InvalidInput;
    if (pose.scene_type == 1 && !owners.scene_one_position_240)
        return S::RequiresScenePositionOwner;
    if (!player_node_identity) return S::RequiresQueueNode;
    // Typed native queue invariants replace intrusive pointer traversal.
    for (size_t i = 0; i < owners.queue.size(); ++i) {
        const auto& item = owners.queue[i];
        if (!item.node_identity || (i && owners.queue[i-1].priority > item.priority))
            return S::InvalidQueue;
        for (size_t j = 0; j < i; ++j)
            if (owners.queue[j].node_identity == item.node_identity) return S::InvalidQueue;
    }
    constexpr uint32_t priority = 0x1010;
    for (const auto& item : owners.queue)
        if (item.node_identity == player_node_identity && item.priority != priority)
            return S::UnsupportedQueuePriorityChange;
    try {
        auto next = owners;
        next.saved_pose = pose;
        if (pose.scene_type == 1) next.scene_one_position_240 = pose.position;
        next.position_802fc20c = pose.position;
        const auto existing = std::find_if(next.queue.begin(), next.queue.end(),
            [player_node_identity](const WorldMapPlayerUpdateQueueEntry& item) {
                return item.node_identity == player_node_identity;
            });
        if (existing != next.queue.end()) next.queue.erase(existing);
        const auto at = std::lower_bound(next.queue.begin(), next.queue.end(), priority,
            [](const WorldMapPlayerUpdateQueueEntry& item, uint32_t key) { return item.priority < key; });
        next.queue.insert(at, {player_node_identity, priority});
        *after = std::move(next);
        return S::Prepared;
    } catch (const std::bad_alloc&) { return S::AllocationFailure; }
}
} // namespace awl
