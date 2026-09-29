#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace awl {

struct WorldMapPackedSavedValues;

struct WorldMapEventConditionEntry {
    const uint8_t* data = nullptr;
    size_t size = 0;
    const char* name = nullptr;
};

struct WorldMapMovementRequestRecord {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t record_index = 0;
    uint32_t saved_condition_id = 0;
};

enum class WorldMapMovementRecordStatus {
    EligibleForStateRequest,
    Rejected,
    RequiresUntranslatedPredicate,
    InvalidInput,
};

struct WorldMapMovementRecordDecision {
    uint32_t action_index = 0;
    uint8_t decoder_flag = 0;
};

// FUN_80015648 supplies the transition values; FUN_80015640 supplies the
// current stage. clock_ticks is the raw word read by FUN_80010058/7C.
struct WorldMapMovementEvaluationState {
    uint8_t state_11cec = 0;
    bool has_time_state = false;
    int32_t current_stage = 0;
    int32_t transition_stage = 0;
    float transition_fraction = 0.0f;
    uint32_t clock_ticks = 0;
};

// Evaluates the bounded local type-3 record shapes. Eligibility still
// precedes FUN_8010AAC4 and never applies an action.
[[nodiscard]] WorldMapMovementRecordStatus evaluate_world_map_movement_record(
    const WorldMapMovementRequestRecord& record,
    const WorldMapMovementEvaluationState& state,
    WorldMapMovementRecordDecision* out);

// Convenience for the slot-0 shape that needs only the global byte.
[[nodiscard]] WorldMapMovementRecordStatus evaluate_world_map_movement_record(
    const WorldMapMovementRequestRecord& record,
    uint8_t state_11cec,
    WorldMapMovementRecordDecision* out);

enum class WorldMapMovementRequestStatus {
    ReadyForActionPath,
    NoEligibleRecord,
    BlockedByOwnerState,
    RequiresUntranslatedPredicate,
    InvalidInput,
};

struct WorldMapMovementRequestPreparation {
    size_t record_index = 0;
    uint32_t action_index = 0;
    uint8_t decoder_flag = 0;
    bool use_global_action_list = false; // true: +0x44E4; false: +0x450C
    int32_t action_list_index = 0;
};

// Owns the ARC selected for owner +0x4538 by FUN_8010A6D8. The type-3
// request header and local slot-0/slot-1 record shapes are decoded; other
// predicate paths remain untranslated.
// Entry views are invalidated by load_phase(), parse(), clear(), or destruction.
class WorldMapEventConditions {
public:
    [[nodiscard]] bool load_phase(uint32_t phase);
    [[nodiscard]] bool parse(std::vector<uint8_t> bytes, uint32_t phase);
    void clear();

    [[nodiscard]] bool loaded() const { return phase_ >= 0; }
    [[nodiscard]] int32_t phase() const { return phase_; }
    [[nodiscard]] size_t entry_count() const { return entries_.size(); }
    [[nodiscard]] bool entry(size_t index,
                             WorldMapEventConditionEntry* out) const;
    // FUN_80126B40's type-3 entry and initial saved-value/slot/value-zero
    // filters only. Matching records still require later condition checks.
    [[nodiscard]] bool select_movement_request_records(
        int32_t slot, const WorldMapPackedSavedValues& saved_values,
        std::vector<WorldMapMovementRequestRecord>* out) const;

    // FUN_8010A9C0 -> FUN_80126B40 -> FUN_8010AAC4 through its entry
    // gate and action-list routing. Does not load an action or mutate state.
    [[nodiscard]] WorldMapMovementRequestStatus prepare_movement_request(
        int32_t slot, const WorldMapPackedSavedValues& saved_values,
        const WorldMapMovementEvaluationState& state,
        int32_t owner_action_680, int32_t owner_mode_58c,
        WorldMapMovementRequestPreparation* out) const;

private:
    struct Entry {
        uint32_t offset = 0;
        uint32_t size = 0;
        std::string name;
    };
    std::vector<uint8_t> bytes_;
    std::vector<Entry> entries_;
    int32_t phase_ = -1;
};

} // namespace awl
