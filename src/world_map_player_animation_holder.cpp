#include "awl/world_map_player_animation_holder.h"
#include <new>
#include <type_traits>
#include <utility>

namespace awl {
namespace {
WorldMapNativeAnimationInitializerMetadata supplied_metadata(const WorldMapPlayerAnimationHolderState& s) {
    WorldMapNativeAnimationInitializerMetadata result;
    auto& a = result.animation;
    a.current_descriptor_0 = s.current_descriptor_0; a.base_descriptor_4 = s.base_descriptor_4;
    a.speed_8 = s.speed_8; a.default_duration_c = s.default_duration_c; a.default_count_14 = s.default_count_14;
    // DB28's supported modes never read these prior words: mode 2 overwrites
    // +10; mode 3 overwrites +18/+1C; D69C overwrites +20/+21 when changed.
    // Private placeholders adapt the existing supplied-state helper. They are
    // never exposed as observations or used to establish preserved fields.
    a.deadline_10 = s.deadline_10.value_or(0); a.count_18 = s.count_18.value_or(0);
    a.completed_count_1c = s.completed_count_1c.value_or(0);
    a.completed_20 = s.completed_20.value_or(0); a.flag_21 = s.flag_21.value_or(0);
    a.restart_deadline_24 = s.restart_deadline_24; a.restart_limit_28 = s.restart_limit_28; a.restart_count_2c = s.restart_count_2c;
    a.model_identity_30 = s.model_identity_30;
    result.has_optional_bindings = true; result.feature_38 = s.feature_38; result.feature_3c = s.feature_3c;
    result.secondary_model_c0 = s.secondary_model_c0; return result;
}
void publish_current_fields(WorldMapPlayerAnimationHolderState& state,
    const WorldMapNativeAnimationInitializerMetadata& metadata, const WorldMapActorAnimationDescriptor& descriptor) noexcept {
    const auto& a = metadata.animation;
    state.flag_21 = a.flag_21;
    const auto mode = (descriptor.word_0 >> 10) & 3u;
    if (mode == 2) state.deadline_10 = a.deadline_10;
    if (mode == 3) { state.count_18 = a.count_18; state.completed_count_1c = a.completed_count_1c; }
    state.feature_38 = metadata.feature_38; state.feature_3c = metadata.feature_3c;
}
void publish_known_fields(WorldMapPlayerAnimationHolderState& state,
    const WorldMapNativeAnimationInitializerMetadata& metadata, const WorldMapActorAnimationDescriptor& descriptor) noexcept {
    publish_current_fields(state,metadata,descriptor);
    const auto& a=metadata.animation;
    state.current_descriptor_0=a.current_descriptor_0;state.base_descriptor_4=a.base_descriptor_4;
    state.completed_20=a.completed_20;state.restart_count_2c=a.restart_count_2c;
}
template<class Result> void diagnostics(Result& out, const WorldMapNativeAnimationInitializerStep& initialized) noexcept {
    out.initializer_status = initialized.initializer_status;
    if (initialized.initializer) {
        out.secondary_index = initialized.initializer->secondary_index;
        out.required_row = initialized.initializer->required_row;
        const auto& step = *initialized.initializer;
        for (const auto* channel : {&step.setup, &step.settings}) if (*channel &&
            ((*channel)->required_record || (*channel)->required_fields)) {
            out.channel_dependency = WorldMapPlayerAnimationHolderChannelDependency{
                channel == &step.settings, (*channel)->required_record, (*channel)->required_fields};
        }
    }
}
using HolderProgress=WorldMapPlayerHolderProgressStatus;
HolderProgress require_current_initializer(WorldMapPlayerAnimationHolderState&,WorldMapActorAnimationDescriptor&) {
    return HolderProgress::RequiresInitialization;
}
HolderProgress require_feature_reset(WorldMapPlayerAnimationHolderState&) {return HolderProgress::RequiresFeatureReset;}
HolderProgress supplied_loop_disable(bool) {return HolderProgress::Prepared;}
template<class Resolve,class Initialize,class Reset,class Disable>
WorldMapPlayerHolderProgressStatus prepare_holder_policy(
    const WorldMapPlayerAnimationHolderState& state, const WorldMapActorAnimationDescriptor& descriptor,
    bool primary_completed, std::optional<uint32_t> now, Resolve resolve, Initialize initialize,
    Reset reset, Disable disable, WorldMapPlayerHolderPolicyStep* out) {
    using S=WorldMapPlayerHolderProgressStatus;
    if (!out || &state==&out->after || &descriptor==&out->descriptor ||
        !state.current_descriptor_0 || state.current_descriptor_0>UINT32_MAX || descriptor.identity!=state.current_descriptor_0) return S::InvalidInput;
    WorldMapPlayerHolderPolicyStep step;step.after=state;step.descriptor=descriptor;
    const auto stop=[&](S status) {
        step.after=state;step.descriptor=descriptor;
        step.disable_primary_loop=false;step.disable_secondary_loop=false;
        *out=step;return status;
    };
    const uint32_t mode=(descriptor.word_0>>10)&3u,clip=descriptor.word_0&1023u;
    bool advance=false;
    if (mode==1) {
        if (primary_completed) step.after.completed_20=uint8_t{1};
    } else {
        if (!state.completed_20) { step.required_field=0x20;return stop(S::RequiresCompletedFlag); }
        if (*state.completed_20) advance=primary_completed && clip!=1023;
        else if (mode==2) {
            if (!now) return stop(S::RequiresClock);
            if (!state.deadline_10) { step.required_field=0x10;return stop(S::RequiresDeadline); }
            advance=*now>=*state.deadline_10;
        } else if (primary_completed) {
            advance=true;
            if (mode==3) {
                if (!state.completed_count_1c) { step.required_field=0x1c;return stop(S::RequiresCount); }
                const uint32_t count=*state.completed_count_1c+1u;
                step.after.completed_count_1c=count;
                if (!state.count_18) { step.required_field=0x18;return stop(S::RequiresCount); }
                if (count+1u==*state.count_18) {
                    step.disable_primary_loop=true;
                    step.disable_secondary_loop=((descriptor.word_4>>11)&127u)!=127;
                    const auto status=disable(step.disable_secondary_loop);
                    if (status==S::InvalidInput) return status;
                    if (status!=S::Prepared) return stop(status);
                }
                advance=count>=*state.count_18;
            }
        }
    }
    if (advance) {
        if (state.current_descriptor_0>UINT32_MAX-12u) return S::InvalidInput;
        const auto required=state.current_descriptor_0+12u;
        const auto next=resolve(required);
        if (!next) { step.required_descriptor=required;return stop(S::RequiresNextDescriptor); }
        if (next->identity!=required) return S::InvalidInput;
        step.after.current_descriptor_0=required;step.descriptor=*next;
        if ((next->word_0&1023u)!=1023) {
            const auto status=initialize(step.after,step.descriptor);
            if (status==S::InvalidInput) return status;
            if (status!=S::Prepared) {step.required_descriptor=required;return stop(status);}
        } else {
            step.after.completed_20=uint8_t{1};
            if (step.after.feature_38) {
                const auto status=reset(step.after);
                if (status==S::InvalidInput) return status;
                if (status!=S::Prepared) return stop(status);
            }
        }
    }
    if ((step.descriptor.word_0&1023u)==1023 || (((step.descriptor.word_0>>10)&3u)==1 && primary_completed)) {
        ++step.after.restart_count_2c;
        bool restart=state.restart_limit_28 && step.after.restart_count_2c<state.restart_limit_28;
        if (!restart && state.restart_deadline_24) {
            if (!now) return stop(S::RequiresClock);
            restart=*now<state.restart_deadline_24;
        }
        if (restart) {
            if (!state.base_descriptor_4 || state.base_descriptor_4>UINT32_MAX) return S::InvalidInput;
            step.after.current_descriptor_0=state.base_descriptor_4;
            const auto status=initialize(step.after,step.descriptor);
            if (status==S::InvalidInput) return status;
            if (status!=S::Prepared) {step.required_descriptor=state.base_descriptor_4;return stop(status);}
        }
    }
    *out=step;return S::Prepared;
}
} // namespace
WorldMapPlayerHolderProgressStatus prepare_world_map_player_holder_policy(
    const WorldMapPlayerAnimationHolderState& state, const WorldMapActorAnimationDescriptor& descriptor,
    bool primary_completed, std::optional<uint32_t> now,
    const std::optional<WorldMapActorAnimationDescriptor>& next, WorldMapPlayerHolderPolicyStep* out) noexcept {
    return prepare_holder_policy(state,descriptor,primary_completed,now,[&](uint64_t){return next;},
        require_current_initializer,require_feature_reset,supplied_loop_disable,out);
}
bool prepare_world_map_player_holder_repeat(const WorldMapPlayerAnimationHolderState& state,
    WorldMapPlayerHolderRepeatKind kind,uint32_t value,std::optional<uint32_t> now,WorldMapPlayerAnimationHolderState* out) noexcept {
    using K=WorldMapPlayerHolderRepeatKind;
    if (!out || (kind!=K::Duration && kind!=K::Count) || (kind==K::Duration && !now)) return false;
    auto after=state;
    if (kind==K::Duration) {after.restart_deadline_24=*now+value;after.restart_limit_28=0;}
    else after.restart_limit_28=value;
    *out=after;return true;
}
bool WorldMapPlayerAnimationHolder::configure_repeats(WorldMapPlayerHolderRepeatKind kind,uint32_t value) noexcept {
    const auto now=kind==WorldMapPlayerHolderRepeatKind::Duration && game_clock_ ?
        std::optional<uint32_t>(game_clock_->state().raw_time):std::nullopt;
    return prepare_world_map_player_holder_repeat(state_,kind,value,now,&state_);
}
WorldMapPlayerHolderProgressResult WorldMapPlayerAnimationHolder::advance_animation() {
    using S=WorldMapPlayerHolderProgressStatus;using C=WorldMapAnimationChannelStatus;
    static_assert(std::is_nothrow_copy_assignable_v<WorldMapPlayerAnimationHolderState>);
    WorldMapPlayerHolderProgressResult result;
    try {
        auto& channel=*primary_channel_;auto& model=primary_->model();const auto binding=model.binding();
        auto pool=channel.records_;pool.push_back({binding.playback_178,model.partial_playback()});
        WorldMapAnimationChannelProgressStep progress;
        result.channel_status=prepare_world_map_partial_animation_channel_progress(channel.state_,pool,binding.model_identity,binding,&progress);
        if (*result.channel_status!=C::Prepared) {
            result.status=S::ChannelIncomplete;result.required_record=progress.update.required_record;
            result.required_playback_fields=progress.update.required_fields;return result;
        }
        result.primary_completed=progress.completed;
        std::optional<WorldMapAnimationChannelProgressStep> secondary_progress;
        if (((descriptor_.word_4>>11)&127u)!=127) {
            if (!secondary_model_ || state_.secondary_model_c0!=secondary_model_->binding().model_identity) {
                result.status=S::RequiresSecondary;return result;
            }
            const auto secondary_binding=secondary_model_->binding();
            auto secondary_pool=secondary_channel_->records_;
            secondary_pool.push_back({secondary_binding.playback_178,secondary_model_->partial_playback()});
            secondary_progress.emplace();
            result.secondary_channel_status=prepare_world_map_partial_animation_channel_progress(secondary_channel_->state_,
                secondary_pool,secondary_binding.model_identity,secondary_binding,&*secondary_progress);
            if (*result.secondary_channel_status!=C::Prepared) {
                result.status=S::ChannelIncomplete;result.required_record=secondary_progress->update.required_record;
                result.required_playback_fields=secondary_progress->update.required_fields;return result;
            }
            result.secondary_completed=secondary_progress->completed;
        }
        const auto now=game_clock_?std::optional<uint32_t>(game_clock_->state().raw_time):std::nullopt;
        const auto resolve=[&](uint64_t key)->std::optional<WorldMapActorAnimationDescriptor> {
            WorldMapActorAnimationDescriptor record;
            if (tables_ && tables_->sequence_record(state_.base_descriptor_4,key,&record)) return record;
            return {};
        };
        const auto observations=[&](const WorldMapPlayerAnimationHolderState& state) {
            WorldMapAnimationInitializerObservations obs;obs.clock=now;
            if (state.feature_38 && timer_assets_) {
                using B=WorldMapPlayerTimerBank;
                obs.fallback_table_38=timer_assets_->table_identity(B::Eyes);obs.fallback_table_24=timer_assets_->table_identity(B::Mouth);
                for (uint32_t channel_index=0;channel_index<2;++channel_index) {
                    const auto type=channel_index?state.feature_38->type_4:state.feature_38->type_0;
                    const uint32_t minimum=channel_index?0x24u:0x3au;
                    WorldMapPlayerTimerResourceView resource;const auto bank=channel_index?B::Mouth:B::Eyes;
                    if (type>=minimum && type<=INT32_MAX && timer_assets_->resource(bank,type-minimum,0,&resource))
                        obs.rows.push_back({timer_assets_->table_identity(bank),type-minimum,0,resource.identity});
                }
            }
            return obs;
        };
        std::optional<WorldMapModelLinkState> links;
        const auto initialize=[&](WorldMapPlayerAnimationHolderState& state,WorldMapActorAnimationDescriptor& descriptor) {
            const auto record=resolve(state.current_descriptor_0);
            if (!record) return S::RequiresInitialization;
            WorldMapPlayerAnimationGroupBinding group;
            if (bind_world_map_player_animation_group(record->word_0,animations_,&group)!=WorldMapPlayerAnimationAssetsStatus::Bound) {
                result.initializer_status=WorldMapAnimationInitializerStatus::RequiresBinding;return S::RequiresInitialization;
            }
            const auto* bank=model.animation_bank(group.primary.bank_identity);
            if (!bank || !bank->same_contents(group.assets->primary_animations())) return S::InvalidInput;
            if (!links) {links.emplace();for (const auto* owner:models_) links->nodes.push_back(owner->model_links());}
            const auto metadata=supplied_metadata(state);
            WorldMapPartialAnimationInitializerState supplied;
            supplied.animation=metadata.animation;supplied.primary=progress.update.after;supplied.records=progress.update.records_after;
            supplied.has_optional_bindings=true;supplied.feature_38=state.feature_38;supplied.feature_3c=state.feature_3c;
            supplied.secondary_model_c0=state.secondary_model_c0;supplied.model_links=links;
            WorldMapPartialAnimationInitializerStep prepared;
            result.initializer_status=prepare_world_map_partial_animation_current_initializer(supplied,record,group.primary,binding,bank,observations(state),&prepared);
            result.required_row=prepared.required_row;result.secondary_index=prepared.secondary_index;
            for (const auto* step:{&prepared.setup,&prepared.settings}) if (*step && ((*step)->required_record || (*step)->required_fields)) {
                result.required_record=(*step)->required_record;result.required_playback_fields=(*step)->required_fields;
            }
            if (*result.initializer_status==WorldMapAnimationInitializerStatus::InvalidInput) return S::InvalidInput;
            if (*result.initializer_status!=WorldMapAnimationInitializerStatus::Prepared) return S::RequiresInitialization;
            if (!prepared.after.model_links || prepared.after.model_links->nodes.size()!=models_.size()) return S::InvalidInput;
            auto after=metadata;after.animation=prepared.after.animation;after.feature_38=prepared.after.feature_38;after.feature_3c=prepared.after.feature_3c;
            publish_current_fields(state,after,*record);descriptor=*record;
            progress.update.after=prepared.after.primary;progress.update.records_after.swap(prepared.after.records);
            links=std::move(prepared.after.model_links);++result.initialized_records;return S::Prepared;
        };
        const auto reset=[&](WorldMapPlayerAnimationHolderState& state) {
            WorldMapAnimationFeatureStep prepared;
            result.feature_status=prepare_world_map_animation_feature38(*state.feature_38,UINT32_MAX,observations(state),&prepared);
            result.required_row=prepared.required_row;
            if (*result.feature_status==WorldMapAnimationFeatureStatus::InvalidInput) return S::InvalidInput;
            if (*result.feature_status!=WorldMapAnimationFeatureStatus::Prepared) return S::RequiresFeatureReset;
            state.feature_38=prepared.after;result.terminal_feature_reset=true;return S::Prepared;
        };
        const auto disable=[&](bool secondary) {
            if (secondary && !secondary_progress) return S::RequiresSecondary;
            // This write precedes any subsequent DB28 setup/settings.
            bool primary_found=false,secondary_found=!secondary;
            for (auto& record:progress.update.records_after) if (record.identity==progress.update.after.target_8) {
                record.state.word_8=0u;primary_found=true;break;
            }
            if (secondary) for (auto& record:secondary_progress->update.records_after)
                if (record.identity==secondary_progress->update.after.target_8) {
                    record.state.word_8=0u;secondary_found=true;break;
                }
            return primary_found && secondary_found?S::Prepared:S::InvalidInput;
        };
        WorldMapPlayerHolderPolicyStep policy;
        result.status=prepare_holder_policy(state_,descriptor_,progress.completed,now,resolve,initialize,reset,disable,&policy);
        result.required_descriptor=policy.required_descriptor;result.required_field=policy.required_field;
        if (result.status!=S::Prepared) return result;
        if (!policy.after.feature_38) result.timers=WorldMapTextureTimerResult{WorldMapTextureTimerStatus::RequiresFeature};
        else {
            WorldMapAnimationFeature38 timers;
            result.timers=prepare_world_map_player_texture_timers(*policy.after.feature_38,policy.after.feature_flag_34,timer_assets_.get(),now,&timers);
            if (result.timers->status==WorldMapTextureTimerStatus::Prepared) policy.after.feature_38=timers;
        }
        if (result.timers->status!=WorldMapTextureTimerStatus::Prepared) { result.status=S::TimerIncomplete;return result; }
        // All fallible work is complete. The last pool record belongs to the
        // model; the channel retains its own records under their stable keys.
        const auto model_after=progress.update.records_after.back().state;
        progress.update.records_after.pop_back();
        model.set_playback(model_after);channel.state_=progress.update.after;channel.records_.swap(progress.update.records_after);
        if (secondary_progress) {
            auto& update=secondary_progress->update;
            secondary_model_->set_playback(update.records_after.back().state);update.records_after.pop_back();
            secondary_channel_->state_=update.after;secondary_channel_->records_.swap(update.records_after);
        }
        if (links) for (size_t i=0;i<models_.size();++i) models_[i]->publish_links(links->nodes[i]);
        state_=policy.after;descriptor_=policy.descriptor;
        result.status=S::Advanced;return result;
    } catch (const std::bad_alloc&) { result.status=S::AllocationFailure;return result; }
}
WorldMapTextureTimerResult WorldMapPlayerAnimationHolder::advance_texture_timers() noexcept {
    using S=WorldMapTextureTimerStatus;
    if (!state_.feature_38) return {S::RequiresFeature};
    WorldMapAnimationFeature38 after;
    const auto now=game_clock_?std::optional<uint32_t>(game_clock_->state().raw_time):std::nullopt;
    const auto result=prepare_world_map_player_texture_timers(*state_.feature_38,state_.feature_flag_34,timer_assets_.get(),now,&after);
    if (result.status==S::Prepared) state_.feature_38=after;
    return result;
}
WorldMapPlayerAnimationHolderResult WorldMapPlayerAnimationHolder::initialize_fresh_pair(
    const WorldMapActorAnimationDescriptor& descriptor,const WorldMapPlayerAnimationGroupBinding& group,
    const WorldMapAnimationInitializerObservations& observations) {
    using S=WorldMapPlayerAnimationHolderStatus;using I=WorldMapAnimationInitializerStatus;
    WorldMapPlayerAnimationHolderResult result;
    result.initializer_status=I::RequiresSecondarySetup;
    const uint32_t index=(descriptor.word_4>>11)&127u;result.secondary_index=index;
    if (state_.secondary_model_c0 || secondary_model_ || state_.feature_c4 || state_.auxiliary_model_c8 || state_.arena_cc || index==127)
        return result;
    const auto& models=group.assets->secondary_models();
    WorldMapSecondarySetupStep construction;
    result.secondary_setup_status=prepare_world_map_secondary_model_setup(index,group.secondary.model_bank_identity,
        &models,0,std::nullopt,{},uint64_t{0},&construction);
    if (*result.secondary_setup_status==WorldMapSecondarySetupStatus::InvalidInput) return result;
    if (*result.secondary_setup_status!=WorldMapSecondarySetupStatus::RequiresConstruction) {
        result.status=S::InitializerIncomplete;return result;
    }
    std::unique_ptr<WorldMapNativeModel> secondary;
    result.secondary_construction=construct_world_map_secondary_model(models,index,construction,&secondary);
    if (result.secondary_construction->status==WorldMapModelConstructionStatus::AllocationFailure) {result.status=S::AllocationFailure;return result;}
    if (result.secondary_construction->status==WorldMapModelConstructionStatus::InvalidInput) return result;
    if (result.secondary_construction->status!=WorldMapModelConstructionStatus::Constructed) {result.status=S::InitializerIncomplete;return result;}
    auto& primary=primary_->model();const auto binding=primary.binding();
    const auto metadata=supplied_metadata(state_);
    WorldMapPartialAnimationInitializerState supplied;
    supplied.animation=metadata.animation;supplied.primary=primary_channel_->state_;supplied.records=primary_channel_->records_;
    supplied.records.push_back({binding.playback_178,primary.partial_playback()});
    supplied.has_optional_bindings=true;supplied.feature_38=state_.feature_38;supplied.feature_3c=state_.feature_3c;
    supplied.model_links.emplace();supplied.model_links->nodes={primary.model_links(),secondary->model_links()};
    WorldMapPartialAnimationSecondaryInput other;
    other.selected_index=index;other.animation_bank_identity=group.secondary_animation_bank_identity;
    other.channel=secondary_channel_->state_;other.records=secondary_channel_->records_;other.model=secondary->binding();
    other.records.push_back({other.model.playback_178,secondary->partial_playback()});other.bank=&group.assets->secondary_animations();
    WorldMapPartialAnimationInitializerStep prepared;
    result.initializer_status=prepare_world_map_partial_animation_fresh_pair_initializer(supplied,descriptor.identity,
        descriptor,group.primary,binding,&group.assets->primary_animations(),observations,other,&prepared);
    result.required_row=prepared.required_row;
    for (const auto* step:{&prepared.setup,&prepared.settings,&prepared.secondary_channel_setup,&prepared.secondary_channel_settings})
        if (*step && ((*step)->required_record || (*step)->required_fields))
            result.channel_dependency=WorldMapPlayerAnimationHolderChannelDependency{
                step==&prepared.settings || step==&prepared.secondary_channel_settings,(*step)->required_record,(*step)->required_fields,
                step==&prepared.secondary_channel_setup || step==&prepared.secondary_channel_settings};
    if (*result.initializer_status==I::InvalidInput) return result;
    if (*result.initializer_status!=I::Prepared) {result.status=S::InitializerIncomplete;return result;}
    if (!prepared.secondary_channel_settings || !prepared.after.model_links || prepared.after.model_links->nodes.size()!=2 ||
        prepared.after.records.empty() || prepared.after.records.back().identity!=binding.playback_178 ||
        prepared.secondary_channel_settings->records_after.empty() || prepared.secondary_channel_settings->records_after.back().identity!=other.model.playback_178)
        return result;
    // Copy immutable banks before publication; models borrow the holder's two
    // existing channels. No replacement channel keys or third channel exist.
    auto primary_banks=primary.animation_banks_;
    primary_banks.push_back(std::make_shared<const WorldMapAnimationBank>(group.assets->primary_animations()));
    secondary->animation_banks_.push_back(std::make_shared<const WorldMapAnimationBank>(*other.bank));
    auto registry=models_;registry.push_back(secondary.get());
    auto& secondary_after=*prepared.secondary_channel_settings;
    primary.set_playback(prepared.after.records.back().state);prepared.after.records.pop_back();
    secondary->set_playback(secondary_after.records_after.back().state);secondary_after.records_after.pop_back();
    primary.animation_banks_.swap(primary_banks);
    primary_channel_->state_=prepared.after.primary;primary_channel_->records_.swap(prepared.after.records);
    secondary_channel_->state_=secondary_after.after;secondary_channel_->records_.swap(secondary_after.records_after);
    primary.publish_links(prepared.after.model_links->nodes[0]);secondary->publish_links(prepared.after.model_links->nodes[1]);
    auto after=metadata;after.animation=prepared.after.animation;after.feature_38=prepared.after.feature_38;after.feature_3c=prepared.after.feature_3c;
    publish_known_fields(state_,after,descriptor);state_.secondary_model_c0=other.model.model_identity;descriptor_=descriptor;
    secondary_model_=std::move(secondary);models_.swap(registry);result.status=S::ConstructedCpuHolder;return result;
}
WorldMapPlayerAnimationHolderResult construct_world_map_player_animation_holder(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& model_assets,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& animation_assets,
    const WorldMapActorAnimationDescriptor& initial_descriptor,
    const std::optional<WorldMapAnimationFeature38>& feature_38,
    const WorldMapAnimationInitializerObservations& observations,
    std::unique_ptr<WorldMapPlayerAnimationHolder>* out) {
    using Status = WorldMapPlayerAnimationHolderStatus;
    using Native = WorldMapNativeAnimationInitializerStatus;
    if (!out || !initial_descriptor.identity) return {};
    if (!model_assets) return {Status::RequiresModelAssets};
    if (!animation_assets) return {Status::RequiresAnimationAssets};
    try {
        WorldMapPlayerAnimationGroupBinding group;
        const auto bound = bind_world_map_player_animation_group(initial_descriptor.word_0, animation_assets, &group);
        if (bound == WorldMapPlayerAnimationAssetsStatus::RequiresGroup) return {Status::RequiresGroup};
        if (bound != WorldMapPlayerAnimationAssetsStatus::Bound) return {};
        auto holder = std::unique_ptr<WorldMapPlayerAnimationHolder>(new WorldMapPlayerAnimationHolder);
        for (auto* channel : {&holder->primary_channel_, &holder->secondary_channel_})
            if (construct_world_map_animation_channel(channel) != WorldMapAnimationChannelConstructionStatus::Constructed) return {Status::AllocationFailure};
        const auto primary = construct_world_map_player_primary(model_assets, &holder->primary_);
        if (primary.status == WorldMapPlayerPrimaryStatus::AllocationFailure) return {Status::AllocationFailure, primary};
        if (primary.status != WorldMapPlayerPrimaryStatus::ConstructedCpuPrimary) return {Status::RequiresPrimary, primary};
        holder->animations_ = animation_assets;
        holder->state_.model_identity_30 = holder->primary_->model().binding().model_identity;
        // Stable native owner key for the retained group table equivalent.
        holder->state_.group_identity_34 = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(animation_assets.get()));
        holder->state_.feature_38 = feature_38;
        holder->models_.push_back(&holder->primary_->model());
        auto metadata = supplied_metadata(holder->state_);
        WorldMapNativeAnimationInitializerStep initialized;
        // Fresh D560 base is null and requested is nonnull. D69C changes it,
        // so the existing helper executes D610's unconditional DB28 call.
        // This does not implement equal-base D610 rebinding on an old holder.
        const auto status = advance_world_map_native_animation_initializer(&metadata, holder->primary_channel_.get(),
            holder->models_, initial_descriptor.identity, initial_descriptor, group.primary,
            &group.assets->primary_animations(), observations, &initialized);
        if (status == Native::AllocationFailure) return {Status::AllocationFailure};
        if (status == Native::InvalidInput || status == Native::Unchanged) return {};
        WorldMapPlayerAnimationHolderResult result;
        diagnostics(result, initialized);
        if (status==Native::Incomplete && result.initializer_status==WorldMapAnimationInitializerStatus::RequiresSecondarySetup &&
            ((initial_descriptor.word_4>>11)&127u)!=127) {
            // The first pass checked primary setup/features before reaching
            // construction. Reprepare from original state with the fresh model;
            // an unaccepted prefix is never resumed or published.
            result=holder->initialize_fresh_pair(initial_descriptor,group,observations);
            if (result.status==Status::ConstructedCpuHolder) *out=std::move(holder);
            return result;
        }
        if (status != Native::Advanced) { result.status = Status::InitializerIncomplete; return result; }
        publish_known_fields(holder->state_, metadata, initial_descriptor); holder->descriptor_ = initial_descriptor;
        *out = std::move(holder); result.status = Status::ConstructedCpuHolder; return result;
    } catch (const std::bad_alloc&) { return {Status::AllocationFailure}; }
}
WorldMapPlayerAnimationHolderStartResult WorldMapPlayerAnimationHolder::start(
    const std::shared_ptr<const WorldMapPlayerStartAnimationTables>& tables,
    const WorldMapPlayerStartAnimationCommand& command,
    const std::optional<WorldMapPlayerStartItemType>& item_type,
    const WorldMapAnimationInitializerObservations& observations) {
    using Status = WorldMapPlayerStartAnimationStatus;
    static_assert(std::is_nothrow_copy_assignable_v<WorldMapPlayerAnimationHolderState>);
    static_assert(std::is_nothrow_copy_assignable_v<WorldMapPlayerAnimationHolderStartResult>);
    try {
        auto metadata = supplied_metadata(state_);
        WorldMapPlayerStartNativeAnimationStep initialized;
        WorldMapPlayerAnimationHolderStartResult result;
        result.status = advance_world_map_player_start_animation(tables, command, item_type, &metadata,
            primary_channel_.get(), models_, animations_, observations, &initialized);
        result.selection = initialized.selection; diagnostics(result, initialized.initializer);
        if (result.status == Status::Advanced) {
            publish_known_fields(state_, metadata, initialized.selection->descriptor);
            descriptor_ = initialized.selection->descriptor; tables_ = tables;
        }
        return result;
    } catch (const std::bad_alloc&) { return {Status::AllocationFailure}; }
}
WorldMapPlayerInitialHolderResult construct_world_map_player_initial_animation_holder(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& model_assets,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& animation_assets,
    const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>& inputs,
    const WorldMapPlayerInitialAnimationQuery& query,
    const WorldMapAnimationInitializerObservations& observations,
    std::unique_ptr<WorldMapPlayerAnimationHolder>* out) {
    using Status = WorldMapPlayerInitialHolderStatus;
    static_assert(std::is_nothrow_copy_assignable_v<WorldMapPlayerInitialHolderResult>);
    if (!out) return {};
    WorldMapPlayerInitialHolderResult result;
    result.initial = prepare_world_map_player_initial_animation(inputs, query, observations);
    if (result.initial.status != WorldMapPlayerInitialAnimationStatus::Prepared) {
        result.status = result.initial.status == WorldMapPlayerInitialAnimationStatus::InvalidInput ?
            Status::InvalidInput : Status::InitialInputsIncomplete;
        return result;
    }
    if (!model_assets) { result.status = Status::RequiresModelAssets; return result; }
    constexpr uint32_t variants[]{0,0,0,1,1,2};
    if (model_assets->variant() != variants[query.phase]) { result.status = Status::PhaseMismatch; return result; }
    std::unique_ptr<WorldMapPlayerAnimationHolder> staging;
    result.holder = construct_world_map_player_animation_holder(model_assets, animation_assets,
        result.initial.selection->descriptor, result.initial.feature, observations, &staging);
    if (result.holder->status == WorldMapPlayerAnimationHolderStatus::AllocationFailure) {
        result.status = Status::AllocationFailure; return result;
    }
    if (result.holder->status != WorldMapPlayerAnimationHolderStatus::ConstructedCpuHolder) {
        result.status = Status::HolderIncomplete; return result;
    }
    staging->initial_inputs_ = inputs;
    staging->state_.feature_source_10 = result.initial.selection->feature_source_10;
    staging->state_.feature_flag_34 = result.initial.selection->feature_flag_34;
    *out = std::move(staging); result.status = Status::ConstructedCpuHolder; return result;
}
WorldMapPlayerTimedInitialHolderResult construct_world_map_player_initial_holder_with_timers(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& model_assets,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& animation_assets,
    const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>& inputs,
    const std::shared_ptr<const WorldMapPlayerTimerAssets>& timers,
    const WorldMapPlayerInitialAnimationQuery& query, const std::optional<uint32_t>& clock,
    std::unique_ptr<WorldMapPlayerAnimationHolder>* out) {
    using Status = WorldMapPlayerTimedInitialHolderStatus;
    static_assert(std::is_nothrow_copy_assignable_v<WorldMapPlayerTimedInitialHolderResult>);
    if (!out) return {};
    WorldMapPlayerTimedInitialHolderResult result;
    if (!inputs) { result.holder.initial.status = WorldMapPlayerInitialAnimationStatus::RequiresInputs; result.status = Status::InitialInputsIncomplete; return result; }
    result.holder.initial = inputs->select(query);
    if (result.holder.initial.status != WorldMapPlayerInitialAnimationStatus::Selected) { result.status = Status::InitialInputsIncomplete; return result; }
    const auto& selection = *result.holder.initial.selection;
    WorldMapPlayerTimerBinding binding;
    result.timer = bind_world_map_player_timer_assets(timers, selection.type_0, selection.type_4, clock, &binding);
    if (result.timer.status != WorldMapPlayerTimerAssetsStatus::Bound) {
        result.status = result.timer.status == WorldMapPlayerTimerAssetsStatus::AllocationFailure ? Status::AllocationFailure :
            result.timer.status == WorldMapPlayerTimerAssetsStatus::InvalidInput ? Status::InvalidInput : Status::TimerInputsIncomplete;
        return result;
    }
    std::unique_ptr<WorldMapPlayerAnimationHolder> staging;
    result.holder = construct_world_map_player_initial_animation_holder(model_assets, animation_assets, inputs, query, binding.observations, &staging);
    if (result.holder.status != WorldMapPlayerInitialHolderStatus::ConstructedCpuHolder) {
        result.status = result.holder.status == WorldMapPlayerInitialHolderStatus::AllocationFailure ? Status::AllocationFailure : Status::HolderIncomplete;
        return result;
    }
    staging->timer_assets_ = binding.assets;
    *out = std::move(staging); result.status = Status::ConstructedCpuHolder; return result;
}
WorldMapPlayerTimedInitialHolderResult construct_world_map_player_initial_holder_with_clock(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& model_assets,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& animation_assets,
    const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>& inputs,
    const std::shared_ptr<const WorldMapPlayerTimerAssets>& timers,
    const WorldMapPlayerInitialAnimationQuery& query, const std::shared_ptr<GameClock>& clock,
    std::unique_ptr<WorldMapPlayerAnimationHolder>* out) {
    if (!out) return {};
    const auto snapshot = clock ? std::optional<uint32_t>(clock->state().raw_time) : std::nullopt;
    std::unique_ptr<WorldMapPlayerAnimationHolder> staging;
    auto result = construct_world_map_player_initial_holder_with_timers(model_assets, animation_assets,
        inputs, timers, query, snapshot, &staging);
    if (result.status == WorldMapPlayerTimedInitialHolderStatus::ConstructedCpuHolder) {
        staging->game_clock_ = clock;
        *out = std::move(staging);
    }
    return result;
}
bool WorldMapPlayerAnimationHolder::finish_initial_model(float scale,
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets) {
    if (!assets || !state_.feature_38 || initial_model_state_) return false;
    WorldMapPlayerInitialModelState next;
    if (assets->file_count() >= 7) {
        WorldMapPlayerModelAssetView view;
        if (!assets->resource(7, &view)) return false;
        next.private_texture_animation = view;
    }
    // B8F8 copies the primary wrapper's private TAM after D610. Its stable
    // raw-byte key replaces the relocated PPC pointer; no TAM body is read.
    const auto table = next.private_texture_animation ?
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(next.private_texture_animation->data)) : 0;
    state_.feature_38->table_c = table;
    if (!primary_->initialize_root_scale(scale)) return false;
    next.initialized_c = 1; // Last supported wrapper write, after all dependencies.
    initial_model_state_ = next;
    return true;
}
WorldMapPlayerTimedInitialHolderResult construct_world_map_player_initial_model_with_clock(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& model_assets,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& animation_assets,
    const std::shared_ptr<const WorldMapPlayerInitialAnimationInputs>& inputs,
    const std::shared_ptr<const WorldMapPlayerTimerAssets>& timers,
    const WorldMapPlayerInitialAnimationQuery& query, const std::shared_ptr<GameClock>& clock,
    std::unique_ptr<WorldMapPlayerAnimationHolder>* out) {
    using Status = WorldMapPlayerTimedInitialHolderStatus;
    if (!out) return {};
    std::unique_ptr<WorldMapPlayerAnimationHolder> staging;
    auto result = construct_world_map_player_initial_holder_with_clock(model_assets, animation_assets,
        inputs, timers, query, clock, &staging);
    if (result.status != Status::ConstructedCpuHolder) return result;
    if (!staging->finish_initial_model(result.holder.initial.selection->scale, model_assets)) {
        result.status = Status::HolderIncomplete; return result;
    }
    *out = std::move(staging); return result;
}
} // namespace awl
