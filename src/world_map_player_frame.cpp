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
template<class Sample, class PostTransform>
WorldMapPlayerFrameStatus evaluate_frame(
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
    // The retained nonnull SKN makes E6D0 unconditionally dereference +14.
    if (!root_feature) return Status::UnsupportedLayout;
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
                if (node.source_record_index>=setup.selection().model.records.size()) return Status::UnsupportedLayout;
                const auto& record=setup.selection().model.records[node.source_record_index];
                if (!node.pose_c || !record.matrix) return Status::UnsupportedLayout;
                *matrix=*record.matrix;
                if (!supported(*matrix)) return Status::UnsupportedNumerics;
            }
            const auto& post=post_transform(i);
            if (post && !concatenate(*matrix,*post,matrix)) return Status::UnsupportedNumerics;
            return Status::Evaluated;
        };
        staged.feature_matrix_writes.resize(setup.features().size());
        staged.feature_write_order.reserve(setup.features().size());
        if (evaluate_nodes) {
            if (request_skin && core.inverse_initial_matrices.size()!=core.nodes.size()) return Status::UnsupportedLayout;
            staged.node_matrices.reserve(core.nodes.size());
            if (request_skin) staged.skin_palette.reserve(core.nodes.size());
            for (size_t i=0;i<core.nodes.size();++i) {
                const auto& node=core.nodes[i];WorldMapModelMatrix matrix;
                const auto status=local_matrix(i,&matrix);
                if (status!=Status::Evaluated) return status;
                if (node.type_0==1 && node.parent_2!=0xffff) {
                    if (node.parent_2>=i) return Status::UnsupportedLayout;
                    if (!concatenate(staged.node_matrices[node.parent_2],matrix,&matrix)) return Status::UnsupportedNumerics;
                }
                staged.node_matrices.push_back(matrix);
                for (uint32_t feature=0;feature<setup.features().size();++feature) if (setup.features()[feature].node==i) {
                    WorldMapModelMatrix world;
                    if (!concatenate(staged.root_matrix,matrix,&world)) return Status::UnsupportedNumerics;
                    staged.feature_matrix_writes[feature]=world;
                    staged.feature_write_order.push_back(feature);
                }
                if (request_skin) {
                    WorldMapModelMatrix skin;
                    if (!concatenate(matrix,core.inverse_initial_matrices[i],&skin)) return Status::UnsupportedNumerics;
                    staged.skin_palette.push_back(skin);
                }
            }
        }
        if (evaluate_nodes && request_skin) {
            const auto status=execute_world_map_player_skin_work(work,staged.skin_palette,previous_output,&staged.vertex_output);
            if (status==WorldMapPlayerSkinExecutionStatus::AllocationFailure) return Status::AllocationFailure;
            if (status==WorldMapPlayerSkinExecutionStatus::UnsupportedNumerics) return Status::UnsupportedNumerics;
            if (status!=WorldMapPlayerSkinExecutionStatus::Executed) return Status::InvalidInput;
            staged.skin_executed=true;
        } else staged.vertex_output=previous_output;
        staged.feature_matrix_writes[*root_feature]=staged.root_matrix;
        staged.feature_write_order.push_back(*root_feature);
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
} // namespace
WorldMapPlayerFrameStatus evaluate_world_map_player_frame(
    const WorldMapPlayerSkinWork& work,const WorldMapPlayerFrameInput& input,
    const std::vector<uint8_t>& previous_output,WorldMapPlayerFrame* out) {
    return evaluate_frame(work,input.root_pose,input.evaluate_nodes,input.request_skin,input.nodes.size(),input.links,
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
    WorldMapPlayerAnimationFrameResult result;
    result.status=evaluate_frame(work,input.root_pose,input.evaluate_nodes,input.request_skin,input.node_post_transforms.size(),input.links,
        [&](size_t i,std::optional<WorldMapAnimationPose>* sampled) {
            WorldMapAnimationPose value{},prior{}; // Unwritten scratch words are not used by pose conversion.
            const auto status=sample_world_map_blended_animation_pose(input.playback,uint32_t(i),records,banks,input.settings,prior,&value);
            if (status==WorldMapAnimationPoseStatus::NoPose) {sampled->reset();return WorldMapPlayerFrameStatus::Evaluated;}
            if (status==WorldMapAnimationPoseStatus::Sampled) {*sampled=value;return WorldMapPlayerFrameStatus::Evaluated;}
            result.failed_node=uint32_t(i);result.sampling_status=status;
            return WorldMapPlayerFrameStatus::RequiresAnimationSampling;
        },[&](size_t i)->const std::optional<WorldMapModelMatrix>& {return input.node_post_transforms[i];},
        previous_output,out);
    return result;
}
} // namespace awl
