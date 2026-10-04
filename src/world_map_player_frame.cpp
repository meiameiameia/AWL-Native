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
} // namespace
WorldMapPlayerFrameStatus evaluate_world_map_player_frame(
    const WorldMapPlayerSkinWork& work,const WorldMapPlayerFrameInput& input,
    const std::vector<uint8_t>& previous_output,WorldMapPlayerFrame* out) {
    using Status=WorldMapPlayerFrameStatus;
    const auto& setup=work.drawing().setup();const auto& core=setup.core_before_features();
    if (!out || previous_output.size()!=work.initial_output().size() ||
        (input.evaluate_nodes && input.nodes.size()!=core.nodes.size())) return Status::InvalidInput;
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
        const auto root_status=pose(input.root_pose,&staged.root_matrix);
        if (root_status!=Status::Evaluated) return root_status;
        staged.feature_matrix_writes.resize(setup.features().size());
        staged.feature_write_order.reserve(setup.features().size());
        if (input.evaluate_nodes) {
            if (input.request_skin && core.inverse_initial_matrices.size()!=core.nodes.size()) return Status::UnsupportedLayout;
            staged.node_matrices.reserve(core.nodes.size());
            if (input.request_skin) staged.skin_palette.reserve(core.nodes.size());
            for (size_t i=0;i<core.nodes.size();++i) {
                const auto& node=core.nodes[i];const auto& observation=input.nodes[i];WorldMapModelMatrix matrix;
                if (observation.sampled_pose) {
                    const auto status=pose(*observation.sampled_pose,&matrix);
                    if (status!=Status::Evaluated) return status;
                } else {
                    if (node.source_record_index>=setup.selection().model.records.size()) return Status::UnsupportedLayout;
                    const auto& record=setup.selection().model.records[node.source_record_index];
                    if (!node.pose_c || !record.matrix) return Status::UnsupportedLayout;
                    matrix=*record.matrix;
                    if (!supported(matrix)) return Status::UnsupportedNumerics;
                }
                if (observation.post_transform && !concatenate(matrix,*observation.post_transform,&matrix)) return Status::UnsupportedNumerics;
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
                if (input.request_skin) {
                    WorldMapModelMatrix skin;
                    if (!concatenate(matrix,core.inverse_initial_matrices[i],&skin)) return Status::UnsupportedNumerics;
                    staged.skin_palette.push_back(skin);
                }
            }
        }
        if (input.evaluate_nodes && input.request_skin) {
            const auto status=execute_world_map_player_skin_work(work,staged.skin_palette,previous_output,&staged.vertex_output);
            if (status==WorldMapPlayerSkinExecutionStatus::AllocationFailure) return Status::AllocationFailure;
            if (status==WorldMapPlayerSkinExecutionStatus::UnsupportedNumerics) return Status::UnsupportedNumerics;
            if (status!=WorldMapPlayerSkinExecutionStatus::Executed) return Status::InvalidInput;
            staged.skin_executed=true;
        } else staged.vertex_output=previous_output;
        staged.feature_matrix_writes[*root_feature]=staged.root_matrix;
        staged.feature_write_order.push_back(*root_feature);
        *out=std::move(staged);return Status::Evaluated;
    } catch (const std::bad_alloc&) { return Status::AllocationFailure; }
}
} // namespace awl
