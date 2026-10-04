#pragma once
#include "awl/world_map_player_draw_commands.h"

namespace awl {
enum class WorldMapPlayerSkinJobKind { Rigid, BlendTwo, Accumulate };
struct WorldMapPlayerSkinSpan { uint32_t offset = 0, size = 0; };
struct WorldMapPlayerSkinJob {
    WorldMapPlayerSkinJobKind kind = WorldMapPlayerSkinJobKind::Rigid;
    uint32_t record_offset = 0; // SKN-relative, not a native pointer.
    uint16_t vertex_count = 0;
    uint8_t prefix = 0;
    std::vector<uint16_t> matrix_indices;
    WorldMapPlayerSkinSpan source; // Complete rounded DMA source extent, including prefix.
    std::optional<WorldMapPlayerSkinSpan> weights, indices;
    WorldMapPlayerSkinSpan output; // Relative to the retained output base; scatter uses size zero.
    std::vector<uint16_t> scatter_indices; // Ordered, duplicates intentionally retained.
};
// 9478 reorders a supplied row-major frame matrix for the paired-single kernels.
// The SKN record's initial matrix words are scratch, not inverse bind matrices.
// Invalid nonfinite input preserves out; this does not evaluate a skeleton.
[[nodiscard]] bool reorder_world_map_player_skin_matrix(
    const WorldMapModelMatrix& frame_matrix,WorldMapModelMatrix* out);

enum class WorldMapPlayerSkinWorkStatus {
    PreparedWork, RequiresAssets, RequiresDrawingPreparation, RequiresSkinTarget,
    UnsupportedLayout, InvalidInput, AllocationFailure,
};
// Owns private CPU workspace and initial output bytes plus bounded skin jobs.
// No locked cache/OS emulation, frame palette evaluation, live model or accepted
// rendering. Initial output is copied from the
// selected retained GPL array; workspace initialization is a native safety choice.
class WorldMapPlayerSkinWork {
public:
    WorldMapPlayerSkinWork(const WorldMapPlayerSkinWork&) = delete;
    WorldMapPlayerSkinWork& operator=(const WorldMapPlayerSkinWork&) = delete;
    const WorldMapPlayerDrawCommands& drawing() const { return *drawing_; }
    const std::vector<WorldMapPlayerSkinJob>& jobs() const { return jobs_; }
    const std::vector<uint8_t>& initial_output() const { return output_; }
    const uint8_t* workspace() const { return workspace_.data()+workspace_offset_; }
    static constexpr uint32_t workspace_size = 0x4000;
    // C668's four 4-KB banks and two alternate position/index/weight triplets.
    static constexpr std::array<uint32_t,4> banks{0,0x1000,0x2000,0x3000};
    static constexpr std::array<std::array<uint32_t,3>,2> triplets{{{0,0x1000,0x1800},{0x2000,0x3000,0x3800}}};
    uint32_t weight_quantization() const { return 0x08040804; } // GQR6: U8, scale 8 both ways.
    uint32_t vertex_quantization() const { return vertex_quantization_; } // GQR7: S16, SKN scale.
    const std::optional<WorldMapPlayerSkinSpan>& clear_before_accumulation() const { return clear_; }
    const std::vector<uint16_t>& flush_indices() const { return flush_indices_; } // C058 cache operations, not writes.
private:
    WorldMapPlayerSkinWork() = default;
    friend WorldMapPlayerSkinWorkStatus prepare_world_map_player_skin_work(
        const std::shared_ptr<const WorldMapPlayerModelAssets>&,std::unique_ptr<WorldMapPlayerSkinWork>*);
    std::unique_ptr<WorldMapPlayerDrawCommands> drawing_;
    std::vector<WorldMapPlayerSkinJob> jobs_;
    std::vector<uint8_t> workspace_,output_;
    size_t workspace_offset_ = 0;
    uint32_t vertex_quantization_ = 0;
    std::optional<WorldMapPlayerSkinSpan> clear_;
    std::vector<uint16_t> flush_indices_;
};
// C668/C080 job/storage preparation only. Full rounded source/output spans,
// matrix palette indices and ordered scatter/flush targets are checked.
// Unsafe/unsupported input or allocation failures preserve the previous owner.
[[nodiscard]] WorldMapPlayerSkinWorkStatus prepare_world_map_player_skin_work(
    const std::shared_ptr<const WorldMapPlayerModelAssets>& assets,std::unique_ptr<WorldMapPlayerSkinWork>* out);

enum class WorldMapPlayerSkinExecutionStatus {
    Executed, InvalidInput, UnsupportedNumerics, AllocationFailure,
};
// C080/C734/C840/CA60 CPU writes with an explicitly supplied row-major frame
// palette (one matrix per core node). First frame uses initial_output(); later
// frames pass the previous result, preserving bytes the original does not write.
// S16 vertex scale comes from SKN; U8 weights divide by 256, never normalize.
// Round-to-nearest and finite normal/zero arithmetic are the supported boundary.
// Failure preserves out; prior_output may alias out. No skeleton/GPU execution.
[[nodiscard]] WorldMapPlayerSkinExecutionStatus execute_world_map_player_skin_work(
    const WorldMapPlayerSkinWork& work,const std::vector<WorldMapModelMatrix>& frame_palette,
    const std::vector<uint8_t>& prior_output,std::vector<uint8_t>* out);
} // namespace awl
