#include "awl/world_map_player_frame.h"
#include <cfenv>
#include <cmath>
#include <new>
#include <utility>

namespace awl {
namespace {
bool supported(float value) { return std::isfinite(value) && std::fpclassify(value)!=FP_SUBNORMAL; }
bool supported(const WorldMapModelMatrix& matrix) {
    for (float value:matrix) if (!supported(value)) return false;
    return true;
}
// 801B7D6C: first multiply, two fused adds, then translation's paired [0,1]
// contribution for columns 2/3 only. Preserve each single-precision rounding.
bool concatenate(const WorldMapModelMatrix& a,const WorldMapModelMatrix& b,WorldMapModelMatrix* out) {
    if (!supported(a) || !supported(b)) return false;
    WorldMapModelMatrix result{};
    for (size_t row=0;row<3;++row) for (size_t column=0;column<4;++column) {
        float value=b[column]*a[row*4];
        if (!supported(value)) return false;
        value=std::fma(b[4+column],a[row*4+1],value);
        if (!supported(value)) return false;
        value=std::fma(b[8+column],a[row*4+2],value);
        if (!supported(value)) return false;
        if (column>=2) value=std::fma(column==3?1.0f:0.0f,a[row*4+3],value);
        if (!supported(value)) return false;
        result[row*4+column]=value;
    }
    *out=result; return true;
}
WorldMapPlayerFrameStatus pose(const std::array<uint32_t,13>& input,WorldMapModelMatrix* out) {
    const auto status=prepare_world_map_model_pose(input,out);
    if (status==WorldMapModelPreparationStatus::RequiresEulerRotation) return WorldMapPlayerFrameStatus::RequiresPoseConversion;
    if (status!=WorldMapModelPreparationStatus::Prepared || !supported(*out)) return WorldMapPlayerFrameStatus::UnsupportedNumerics;
    return WorldMapPlayerFrameStatus::Evaluated;
}
template<class Sample, class PostTransform, class FeatureAtNode, class ExecuteSkin>
WorldMapPlayerFrameStatus evaluate_frame(
    const WorldMapModelCore& core,const WorldMapPreparedModelResource& resource,
    size_t feature_count,std::optional<uint32_t> root_feature,bool has_skin,
    FeatureAtNode&& feature_at_node,ExecuteSkin&& execute_skin,const WorldMapAnimationPose& root_pose,
    bool evaluate_nodes,bool request_skin,size_t node_count,const WorldMapPlayerFrameLinks& links,
    Sample&& sample,PostTransform&& post_transform,
    const std::vector<uint8_t>& previous_output,WorldMapPlayerFrame* out) {
    using Status=WorldMapPlayerFrameStatus;
    bool has_children=false;
    for (const auto& child:links.children) if (child.child) has_children=true;
    if (!out ||
        ((evaluate_nodes || has_children) && node_count!=core.nodes.size()) ||
        (has_children && !links.identity)) return Status::InvalidInput;
    if (std::fegetround()!=FE_TONEAREST) return Status::UnsupportedNumerics;
    try {
        WorldMapPlayerFrame staged;
        staged.root_pose_after=root_pose;
        const auto& inherited=links.inherited;
        if (inherited.parent) {
            // E488/E4B8 assertion states are explicit native stops.
            if (!inherited.producer || inherited.parent!=inherited.producer || !inherited.flags)
                return Status::UnsupportedLayout;
            if (*inherited.flags&8u) {
                staged.root_pose_after[0]=(root_pose[0]&0x00ffffffu)|(root_pose[0]&0x01000000u);
                const auto status=pose(staged.root_pose_after,&staged.root_matrix);
                if (status!=Status::Evaluated) return status;
                if (!concatenate(inherited.matrix,staged.root_matrix,&staged.root_matrix)) return Status::UnsupportedNumerics;
            } else {
                if (!supported(inherited.matrix)) return Status::UnsupportedNumerics;
                staged.root_matrix=inherited.matrix; // Root pose is unread here.
            }
        } else {
            const auto status=pose(root_pose,&staged.root_matrix);
            if (status!=Status::Evaluated) return status;
        }
        // 01F8 -> pose/default -> optional node +10. EB80 reuses this in
        // child-to-parent order, independently of the E438 type-one rule.
        auto local_matrix=[&](size_t i,WorldMapModelMatrix* matrix) {
            const auto& node=core.nodes[i];
            std::optional<WorldMapAnimationPose> sampled_pose;
            const auto sample_status=sample(i,&sampled_pose);
            if (sample_status!=Status::Evaluated) return sample_status;
            if (sampled_pose) {
                const auto status=pose(*sampled_pose,matrix);
                if (status!=Status::Evaluated) return status;
            } else {
                if (node.source_record_index>=resource.records.size()) return Status::UnsupportedLayout;
                const auto& record=resource.records[node.source_record_index];
                if (!node.pose_c || !record.matrix) return Status::UnsupportedLayout;
                *matrix=*record.matrix;
                if (!supported(*matrix)) return Status::UnsupportedNumerics;
            }
            const auto& post=post_transform(i);
            if (post && !concatenate(*matrix,*post,matrix)) return Status::UnsupportedNumerics;
            return Status::Evaluated;
        };
        staged.feature_matrix_writes.resize(feature_count);
        staged.feature_write_order.reserve(feature_count);
        if (evaluate_nodes) {
            if (has_skin && request_skin && core.inverse_initial_matrices.size()!=core.nodes.size()) return Status::UnsupportedLayout;
            staged.node_matrices.reserve(core.nodes.size());
            if (has_skin && request_skin) staged.skin_palette.reserve(core.nodes.size());
            for (size_t i=0;i<core.nodes.size();++i) {
                const auto& node=core.nodes[i];WorldMapModelMatrix matrix;
                const auto status=local_matrix(i,&matrix);
                if (status!=Status::Evaluated) return status;
                if (node.type_0==1 && node.parent_2!=0xffff) {
                    if (node.parent_2>=i) return Status::UnsupportedLayout;
                    if (!concatenate(staged.node_matrices[node.parent_2],matrix,&matrix)) return Status::UnsupportedNumerics;
                }
                staged.node_matrices.push_back(matrix);
                for (uint32_t feature=0;feature<feature_count;++feature) if (feature_at_node(feature,i)) {
                    WorldMapModelMatrix world;
                    if (!concatenate(staged.root_matrix,matrix,&world)) return Status::UnsupportedNumerics;
                    staged.feature_matrix_writes[feature]=world;
                    staged.feature_write_order.push_back(feature);
                }
                if (has_skin && request_skin) {
                    WorldMapModelMatrix skin;
                    if (!concatenate(matrix,core.inverse_initial_matrices[i],&skin)) return Status::UnsupportedNumerics;
                    staged.skin_palette.push_back(skin);
                }
            }
        }
        if (has_skin && evaluate_nodes && request_skin) {
            const auto status=execute_skin(staged.skin_palette,previous_output,&staged.vertex_output);
            if (status==WorldMapPlayerSkinExecutionStatus::AllocationFailure) return Status::AllocationFailure;
            if (status==WorldMapPlayerSkinExecutionStatus::UnsupportedNumerics) return Status::UnsupportedNumerics;
            if (status!=WorldMapPlayerSkinExecutionStatus::Executed) return Status::InvalidInput;
            staged.skin_executed=true;
        } else staged.vertex_output=previous_output;
        if (root_feature) {
            staged.feature_matrix_writes[*root_feature]=staged.root_matrix;
            staged.feature_write_order.push_back(*root_feature);
        }
        for (size_t slot=0;slot<links.children.size();++slot) {
            const auto& child=links.children[slot];
            if (!child.child) continue;
            if (child.node>=core.nodes.size()) return Status::UnsupportedLayout;
            WorldMapModelMatrix attachment;
            if (evaluate_nodes) attachment=staged.node_matrices[child.node];
            else {
                // EB80 walks every parent regardless of node type. Its final
                // local-root multiplication precedes E438's root multiplication.
                size_t node=child.node,depth=0;bool accumulated=false;
                for (;;) {
                    if (node>=core.nodes.size() || depth++>=core.nodes.size()) return Status::UnsupportedLayout;
                    WorldMapModelMatrix local;
                    const auto status=local_matrix(node,&local);
                    if (status!=Status::Evaluated) return status;
                    if (accumulated) {
                        if (!concatenate(local,attachment,&attachment)) return Status::UnsupportedNumerics;
                    } else {attachment=local;accumulated=true;}
                    if (core.nodes[node].parent_2==0xffff) break;
                    node=core.nodes[node].parent_2;
                }
                WorldMapModelMatrix local_root;
                const auto status=pose(staged.root_pose_after,&local_root);
                if (status!=Status::Evaluated) return status;
                if (!concatenate(local_root,attachment,&attachment)) return Status::UnsupportedNumerics;
            }
            WorldMapPlayerAttachmentWrite write;write.child=child.child;write.producer=links.identity;
            if (!concatenate(staged.root_matrix,attachment,&write.matrix)) return Status::UnsupportedNumerics;
            staged.attachment_writes[slot]=write;
        }
        // E768 reads recursion flags only after all four propagation slots.
        for (size_t slot=0;slot<links.children.size();++slot) if (staged.attachment_writes[slot]) {
            if (!links.children[slot].child_flags) return Status::UnsupportedLayout;
            for (size_t prior=0;prior<slot;++prior) if (links.children[prior].child==links.children[slot].child &&
                links.children[prior].child_flags!=links.children[slot].child_flags) return Status::InvalidInput;
            staged.attachment_writes[slot]->evaluate_child=(*links.children[slot].child_flags&1u)!=0;
        }
        *out=std::move(staged);return Status::Evaluated;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
template<class Sample,class PostTransform>
WorldMapPlayerFrameStatus evaluate_primary(
    const WorldMapPlayerSkinWork& work,const WorldMapAnimationPose& root_pose,
    bool evaluate_nodes,bool request_skin,size_t node_count,const WorldMapPlayerFrameLinks& links,
    Sample&& sample,PostTransform&& post_transform,
    const std::vector<uint8_t>& previous_output,WorldMapPlayerFrame* out) {
    using Status=WorldMapPlayerFrameStatus;
    const auto& setup=work.drawing().setup();const auto& core=setup.core_before_features();
    bool has_children=false;
    for (const auto& child:links.children) if (child.child) has_children=true;
    if (!out || previous_output.size()!=work.initial_output().size() ||
        ((evaluate_nodes || has_children) && node_count!=core.nodes.size()) ||
        (has_children && !links.identity)) return Status::InvalidInput;
    if (std::fegetround()!=FE_TONEAREST) return Status::UnsupportedNumerics;
    if (core.parent_150 || core.word_154) return Status::UnsupportedLayout;
    for (auto child:core.children_15c) if (child) return Status::UnsupportedLayout;
    std::optional<uint32_t> root_feature;
    for (uint32_t i=0;i<setup.features().size();++i) if (!setup.features()[i].node) {
        if (root_feature) return Status::UnsupportedLayout;
        root_feature=i;
    }
    if (!root_feature) return Status::UnsupportedLayout; // E6D0 nonnull SKN root dereference.
    return evaluate_frame(core,setup.selection().model,setup.features().size(),root_feature,true,
        [&](uint32_t feature,size_t node) {return setup.features()[feature].node==node;},
        [&](const auto& palette,const auto& previous,auto* output) {
            return execute_world_map_player_skin_work(work,palette,previous,output);
        },root_pose,evaluate_nodes,request_skin,node_count,links,
        std::forward<Sample>(sample),std::forward<PostTransform>(post_transform),previous_output,out);
}
template<class Sample,class Evaluate>
WorldMapPlayerAnimationFrameResult sampled_animation_frame(Sample&& sample, Evaluate&& evaluate) {
    WorldMapPlayerAnimationFrameResult result;
    result.status=evaluate([&](size_t i,std::optional<WorldMapAnimationPose>* sampled) {
        WorldMapAnimationPose value{},prior{};
        const auto status=sample(uint32_t(i),prior,&value);
        if (status==WorldMapAnimationPoseStatus::NoPose) {sampled->reset();return WorldMapPlayerFrameStatus::Evaluated;}
        if (status==WorldMapAnimationPoseStatus::Sampled) {*sampled=value;return WorldMapPlayerFrameStatus::Evaluated;}
        result.failed_node=uint32_t(i);result.sampling_status=status;
        return WorldMapPlayerFrameStatus::RequiresAnimationSampling;
    });
    return result;
}
template<class Evaluate>
WorldMapPlayerAnimationFrameResult animation_frame(
    const WorldMapPlayerAnimationFrameInput& input,
    const std::vector<WorldMapAnimationPlaybackRecord>& records,
    const std::vector<const WorldMapAnimationBank*>& banks,Evaluate&& evaluate) {
    return sampled_animation_frame([&](uint32_t node,const auto& prior,auto* output) {
        return sample_world_map_blended_animation_pose(input.playback,node,records,banks,input.settings,prior,output);
    },std::forward<Evaluate>(evaluate));
}
template<class Sample>
WorldMapPlayerFrameStatus evaluate_model(
    const WorldMapPreparedModelResource& resource,const WorldMapModelCore& core,
    const WorldMapPlayerAnimationFrameInput& input,Sample&& sample,
    const std::vector<uint8_t>& previous_output,WorldMapPlayerFrame* out) {
    using Status=WorldMapPlayerFrameStatus;
    if (core.nodes.size()!=resource.metadata.count_6 ||
        core.resource.reference.bank_identity!=resource.metadata.reference.bank_identity ||
        core.resource.reference.offset!=resource.metadata.reference.offset || core.auxiliary_c)
        return Status::UnsupportedLayout; // This API requires known null +C.
    return evaluate_frame(core,resource,core.nodes.size(),std::nullopt,false,
        [&](uint32_t feature,size_t node) {return feature==node && core.nodes[node].feature_8!=0;},
        [](const auto&,const auto&,auto*) {return WorldMapPlayerSkinExecutionStatus::InvalidInput;},
        input.root_pose,input.evaluate_nodes,input.request_skin,input.node_post_transforms.size(),input.links,
        std::forward<Sample>(sample),[&](size_t i)->const std::optional<WorldMapModelMatrix>& {return input.node_post_transforms[i];},previous_output,out);
}
} // namespace
WorldMapPlayerFrameStatus evaluate_world_map_player_frame(
    const WorldMapPlayerSkinWork& work,const WorldMapPlayerFrameInput& input,
    const std::vector<uint8_t>& previous_output,WorldMapPlayerFrame* out) {
    return evaluate_primary(work,input.root_pose,input.evaluate_nodes,input.request_skin,input.nodes.size(),input.links,
        [&](size_t i,std::optional<WorldMapAnimationPose>* sampled) {
            *sampled=input.nodes[i].sampled_pose; return WorldMapPlayerFrameStatus::Evaluated;
        },[&](size_t i)->const std::optional<WorldMapModelMatrix>& {return input.nodes[i].post_transform;},
        previous_output,out);
}
WorldMapPlayerAnimationFrameResult evaluate_world_map_player_animation_frame(
    const WorldMapPlayerSkinWork& work,const WorldMapPlayerAnimationFrameInput& input,
    const std::vector<WorldMapAnimationPlaybackRecord>& records,
    const std::vector<const WorldMapAnimationBank*>& banks,
    const std::vector<uint8_t>& previous_output,WorldMapPlayerFrame* out) {
    return animation_frame(input,records,banks,[&](auto&& sample) {
        return evaluate_primary(work,input.root_pose,input.evaluate_nodes,input.request_skin,input.node_post_transforms.size(),input.links,
            sample,[&](size_t i)->const std::optional<WorldMapModelMatrix>& {return input.node_post_transforms[i];},previous_output,out);
    });
}
WorldMapPlayerAnimationFrameResult evaluate_world_map_model_animation_frame(
    const WorldMapPreparedModelResource& resource,const WorldMapModelCore& core,
    const WorldMapPlayerAnimationFrameInput& input,
    const std::vector<WorldMapAnimationPlaybackRecord>& records,
    const std::vector<const WorldMapAnimationBank*>& banks,
    const std::vector<uint8_t>& previous_output,WorldMapPlayerFrame* out) {
    return animation_frame(input,records,banks,[&](auto&& sample) {
        return evaluate_model(resource,core,input,sample,previous_output,out);
    });
}
WorldMapPlayerFrameOwner::WorldMapPlayerFrameOwner(
    std::unique_ptr<WorldMapPlayerSkinWork> work,std::vector<WorldMapModelMatrix> features)
    :work_(std::move(work)),features_(std::move(features)) {}
WorldMapPlayerFrameOwnerResult prepare_world_map_player_frame_owner(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets,std::unique_ptr<WorldMapPlayerFrameOwner>* out) {
    using Status=WorldMapPlayerFrameOwnerStatus;
    if (!out) return {};
    try {
        std::unique_ptr<WorldMapPlayerSkinWork> work;
        const auto result=prepare_world_map_player_skin_work(assets,&work);
        if (result==WorldMapPlayerSkinWorkStatus::AllocationFailure) return {Status::AllocationFailure,result};
        if (result!=WorldMapPlayerSkinWorkStatus::PreparedWork) return {Status::RequiresSkinWork,result};
        std::vector<WorldMapModelMatrix> features;
        features.reserve(work->drawing().setup().features().size());
        for (const auto& feature:work->drawing().setup().features()) features.push_back(feature.matrix);
        auto owner=std::unique_ptr<WorldMapPlayerFrameOwner>(new WorldMapPlayerFrameOwner(std::move(work),std::move(features)));
        *out=std::move(owner);return {Status::PreparedCpuState};
    } catch (const std::bad_alloc&) { return {Status::AllocationFailure}; }
}
WorldMapPlayerAnimationFrameResult WorldMapPlayerFrameOwner::advance(
    const WorldMapPlayerOwnedFrameInput& input,
    const std::vector<WorldMapAnimationPartialPlaybackRecord>& records,
    const std::vector<const WorldMapAnimationBank*>& banks) {
    using Status=WorldMapPlayerFrameStatus;
    // Publishing a primary prefix would accept only part of E438 recursion.
    for (const auto& child:input.links.children) if (child.child) return {Status::RequiresHierarchy};
    try {
        const auto& root=input.root_pose?*input.root_pose:root_pose_;
        WorldMapPlayerFrame staged;
        auto result=sampled_animation_frame([&](uint32_t node,const auto& prior,auto* output) {
            return sample_world_map_partial_blended_animation_pose(input.playback,node,records,banks,input.settings,prior,output);
        },[&](auto&& sample) {
            return evaluate_primary(*work_,root,input.evaluate_nodes,input.request_skin,input.node_post_transforms.size(),input.links,sample,
                [&](size_t i)->const std::optional<WorldMapModelMatrix>& { return input.node_post_transforms[i]; },vertex_output(),&staged);
        });
        if (result.status!=Status::Evaluated) return result;
        // 2B48 feature writes persist. E438's matrices/palette are temporary;
        // an unwritten feature must retain its preceding constructor/frame value.
        auto features_after=features_;
        for (uint32_t index:staged.feature_write_order) {
            if (index>=features_after.size() || index>=staged.feature_matrix_writes.size() || !staged.feature_matrix_writes[index])
                return {Status::UnsupportedLayout};
            features_after[index]=*staged.feature_matrix_writes[index];
        }
        // All allocation/conversion/sampling/skin work completed. These moves
        // cannot throw and publish the pose, features and vertices together.
        root_pose_=staged.root_pose_after;
        features_.swap(features_after);frame_.emplace(std::move(staged));
        return result;
    } catch (const std::bad_alloc&) { return {Status::AllocationFailure}; }
}
WorldMapModelHierarchyFrameResult evaluate_world_map_model_hierarchy_frame(
    const std::vector<WorldMapModelFrameSource>& sources,uint64_t root,
    const std::vector<WorldMapAnimationPlaybackRecord>& records,
    const std::vector<const WorldMapAnimationBank*>& banks,
    size_t maximum_evaluations,WorldMapModelHierarchyFrame* out) {
    using Status=WorldMapModelHierarchyFrameStatus;
    if (!out || !root || !maximum_evaluations) return {};
    auto identity=[](const WorldMapModelFrameSource& source) {
        return source.secondary?source.secondary->binding().model_identity:source.input.links.identity;
    };
    auto find=[&](uint64_t key) {
        for (size_t i=0;i<sources.size();++i) if (identity(sources[i])==key) return i;
        return sources.size();
    };
    for (size_t i=0;i<sources.size();++i) {
        const auto key=identity(sources[i]);
        if (!key || find(key)!=i) return {};
    }
    const size_t root_index=find(root);
    if (root_index==sources.size()) return {Status::RequiresModel,root};
    try {
        WorldMapModelHierarchyFrame staged;staged.models.reserve(sources.size());
        bool has_owned=false;
        for (const auto& source:sources) {
            auto inherited=source.input.links.inherited;
            if (source.secondary) {
                inherited.parent=source.secondary->core().parent_150;
                inherited.flags=source.secondary->core().flags_158;has_owned=true;
            }
            staged.models.push_back({identity(source),source.input.root_pose,inherited,source.previous_frame});
        }
        std::vector<WorldMapAnimationPartialPlaybackRecord> owned_records;
        std::vector<const WorldMapAnimationBank*> owned_banks;
        if (has_owned) {
            for (const auto& record:records) owned_records.push_back({record.identity,partial_world_map_animation_playback(record.state)});
            auto add_bank=[&](const WorldMapAnimationBank* bank) {
                if (!bank || !bank->loaded()) return;
                for (const auto* prior:owned_banks) if (prior->same_contents(*bank)) return;
                // Conflicting bytes remain ambiguous only when this key is read.
                owned_banks.push_back(bank);
            };
            for (const auto& source:sources) if (source.secondary) {
                const auto binding=source.secondary->binding();
                for (const auto& record:owned_records) if (record.identity==binding.playback_178)
                    return {Status::InvalidInput,binding.model_identity};
                owned_records.push_back({binding.playback_178,source.secondary->partial_playback()});
                for (const auto& bank:source.secondary->animation_banks()) add_bank(bank.get());
            }
            for (const auto* bank:banks) add_bank(bank);
        }
        struct Visit {size_t model=0,next_slot=0;};
        std::vector<Visit> stack;
        auto enter=[&](size_t index)->WorldMapModelHierarchyFrameResult {
            const auto& source=sources[index];auto& state=staged.models[index];
            for (const auto& active:stack) if (active.model==index) return {Status::Cycle,state.identity};
            if (staged.evaluation_order.size()>=maximum_evaluations) return {Status::EvaluationLimit,state.identity};
            if ((source.primary && (source.resource || source.core || source.secondary)) ||
                (source.secondary && (source.resource || source.core)) ||
                (!source.primary && !source.secondary && (!source.resource || !source.core))) return {Status::InvalidInput,state.identity};
            auto input=source.input;input.root_pose=state.root_pose;input.links.inherited=state.inherited;
            input.evaluate_nodes=sources[root_index].input.evaluate_nodes;
            input.request_skin=sources[root_index].input.request_skin;
            if (source.secondary) {
                input.links.identity=state.identity;
                for (size_t slot=0;slot<4;++slot) input.links.children[slot]={source.secondary->core().children_15c[slot],
                    source.secondary->core().attachments_16c[slot],std::nullopt};
            }
            // Resolve reached children even when recursion is off: every nonnull
            // slot receives +120/+154. Flags come from the child's actual snapshot.
            for (auto& child:input.links.children) if (child.child) {
                const size_t target=find(child.child);
                if (target==sources.size()) return {Status::RequiresModel,child.child};
                const auto flags=staged.models[target].inherited.flags;
                if (child.child_flags && child.child_flags!=flags) return {Status::InvalidInput,child.child};
                child.child_flags=flags;
            }
            WorldMapPlayerAnimationFrameResult result;
            if (has_owned) {
                const auto playback=source.secondary?source.secondary->partial_playback():partial_world_map_animation_playback(input.playback);
                result=sampled_animation_frame([&](uint32_t node,const auto& prior,auto* output) {
                    return sample_world_map_partial_blended_animation_pose(playback,node,owned_records,owned_banks,input.settings,prior,output);
                },[&](auto&& sample) {
                    if (source.primary) return evaluate_primary(*source.primary,input.root_pose,input.evaluate_nodes,input.request_skin,
                        input.node_post_transforms.size(),input.links,sample,
                        [&](size_t i)->const std::optional<WorldMapModelMatrix>& {return input.node_post_transforms[i];},state.frame.vertex_output,&state.frame);
                    return source.secondary?evaluate_model(source.secondary->prepared_resource(),source.secondary->core(),input,sample,state.frame.vertex_output,&state.frame):
                        evaluate_model(*source.resource,*source.core,input,sample,state.frame.vertex_output,&state.frame);
                });
            } else result=source.primary?
                evaluate_world_map_player_animation_frame(*source.primary,input,records,banks,state.frame.vertex_output,&state.frame):
                evaluate_world_map_model_animation_frame(*source.resource,*source.core,input,records,banks,state.frame.vertex_output,&state.frame);
            if (result.status==WorldMapPlayerFrameStatus::AllocationFailure) return {Status::AllocationFailure,state.identity};
            if (result.status!=WorldMapPlayerFrameStatus::Evaluated) return {Status::FrameFailure,state.identity,result};
            state.root_pose=state.frame.root_pose_after;
            for (const auto& write:state.frame.attachment_writes) if (write) {
                auto& child=staged.models[find(write->child)];
                child.inherited.matrix=write->matrix;child.inherited.producer=state.identity;
            }
            staged.evaluation_order.push_back(state.identity);stack.push_back({index,0});
            return {Status::Evaluated};
        };
        auto result=enter(root_index);
        if (result.status!=Status::Evaluated) return result;
        while (!stack.empty()) {
            auto& visit=stack.back();
            if (visit.next_slot==4) {stack.pop_back();continue;}
            const auto& write=staged.models[visit.model].frame.attachment_writes[visit.next_slot++];
            if (!write || !write->evaluate_child) continue;
            result=enter(find(write->child));
            if (result.status!=Status::Evaluated) return result;
        }
        *out=std::move(staged);return {Status::Evaluated};
    } catch (const std::bad_alloc&) {return {Status::AllocationFailure};}
}
} // namespace awl
