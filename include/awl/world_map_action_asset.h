#pragma once

#include "awl/world_map_event_conditions.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace awl {

// FUN_8017837C/80178450, bounded to the single-block CLZ version-zero
// shape used by the two currently selected Common.arc entries.
// output_limit bounds allocation. Failure preserves the caller's output.
[[nodiscard]] bool decode_world_map_action_clz(
    const uint8_t* data, size_t size, size_t output_limit,
    std::vector<uint8_t>* out);

struct WorldMapActionInstruction {
    uint8_t opcode = 0;
    uint8_t flags = 0;
    uint16_t reserved = 0;
    uint32_t operand = 0;
};

// Only fields reset/written by FUN_80185DBC -> FUN_80185FA4. Native
// ownership replaces code/string pointers; game-owned runtime state is absent.
struct WorldMapActionScriptState {
    uint32_t state_4 = 0;
    uint32_t instruction_index_0c = 0;
    uint32_t instruction_count_10 = 0;
    uint32_t string_count_14 = 0;
    std::array<uint32_t, 100> stack_20{};
    uint8_t stack_depth_1b0 = 0;
    uint32_t options_1b4 = 0;
    std::array<uint32_t, 200> variables_1b8{};
    uint32_t operand_base_4d8 = 0;
    uint32_t mode_flags_528 = 0;
};

enum class WorldMapActionStepStatus {
    Advanced,
    Yielded,
    Halted,
    NotRunning,
    RequiresCallback,
    UnsupportedOpcode,
    InvalidState,
    InvalidOperand,
};

struct WorldMapActionStep {
    uint32_t instruction_index = 0;
    uint32_t effective_operand = 0;
    uint8_t opcode = 0;
    std::array<uint32_t, 8> callback_arguments{};
};

enum class WorldMapCommandPreparationStatus {
    Prepared,
    RequiresManagerSnapshot,
    UnsupportedCommand,
    InvalidState,
    InvalidOperand,
};

enum class WorldMapCommandEffect {
    BeginRequest4,
    PollRequest4,
    CompleteRequest4,
    LevelTransition65,
    IndexedLevel65,
};

// Supplied fields read by command 4. This does not own the request manager.
struct WorldMapCommand4Snapshot {
    uint32_t key_530 = UINT32_MAX;
    uint32_t key_534 = UINT32_MAX;
    bool has_manager_state = false;
    uint32_t manager_state_48 = 0;
    uint32_t manager_result_1c0 = UINT32_MAX;
};

struct WorldMapCommandPreparation {
    WorldMapActionStep instruction{};
    WorldMapActionScriptState after_arguments{};
    WorldMapCommandEffect effect = WorldMapCommandEffect::BeginRequest4;
    uint32_t next_key_530 = UINT32_MAX;
    uint32_t next_key_534 = UINT32_MAX;
    bool has_stack_result = false;
    uint32_t stack_result = 0;
    float normalized_level = 0;
    float transition_seconds = 0;
    uint8_t indexed_target = 0;
    uint8_t indexed_level = 0;
    uint32_t transition_milliseconds = 0;
};

// Bounded RIFF/SCR container from FUN_80185FA4. CODE and OPT are required;
// STR is optional, with raw offsets kept opaque.
class WorldMapActionScript {
public:
    [[nodiscard]] bool parse(std::vector<uint8_t> bytes);
    void clear();
    [[nodiscard]] bool loaded() const { return !bytes_.empty(); }
    [[nodiscard]] uint32_t instruction_count() const { return code_count_; }
    [[nodiscard]] uint32_t string_count() const { return string_count_; }
    [[nodiscard]] uint32_t options() const { return options_; }
    [[nodiscard]] bool instruction(size_t index,
                                    WorldMapActionInstruction* out) const;
    // Resets represented fields only after a successful parse. Failure
    // preserves state. The supplied mode flags correspond to r5 at entry.
    [[nodiscard]] bool initialize_state(uint32_t mode_flags,
                                         WorldMapActionScriptState* out) const;
    // Bounded FUN_80186208 stack/integer/control subset. Each successful step
    // commits supplied state; errors and callback boundaries preserve it.
    // Multiply/divide/remainder and comparison constants remain unsupported.
    [[nodiscard]] WorldMapActionStepStatus step(
        WorldMapActionScriptState* state, WorldMapActionStep* out) const;
    // Composes the common step with only the verified world-map command 0
    // (yield) and 66 (integer argument consumption, no further effects).
    // Yield keeps script state running; the caller must end its current pass.
    // Other commands preserve state and report RequiresCallback.
    [[nodiscard]] WorldMapActionStepStatus step_world_map(
        WorldMapActionScriptState* state, WorldMapActionStep* out) const;
    // Command 4 only: consumes a busy-manager rejection of new keys, a
    // same-key pending poll, or a same-key completed result. Only the supplied
    // interpreter keys change in command4; manager fields remain read-only.
    // Idle new requests require untranslated resources and preserve all state.
    // This does not begin a request or connect a live request manager.
    [[nodiscard]] WorldMapActionStepStatus step_world_map_request(
        WorldMapActionScriptState* state, WorldMapCommand4Snapshot* command4,
        WorldMapActionStep* out) const;
    // Prepares command 4/65 arguments and effect routing without consuming the
    // caller's state or accepting a backend effect. after_arguments is only a
    // supplied snapshot; it is not a completed command or gameplay acceptance.
    [[nodiscard]] WorldMapCommandPreparationStatus prepare_world_map_command(
        const WorldMapActionScriptState& state,
        const WorldMapCommand4Snapshot* command4,
        WorldMapCommandPreparation* out) const;

private:
    std::vector<uint8_t> bytes_;
    size_t code_offset_ = 0;
    uint32_t code_count_ = 0;
    uint32_t string_count_ = 0;
    uint32_t options_ = 0;
};

// Owns the flat /files/Common.arc selected for owner +0x44E4. Indices
// retain the DOL's ARC node numbering, including the non-file root at zero.
// This does not start an action or scene.
class WorldMapGlobalActionArchive {
public:
    [[nodiscard]] bool load();
    [[nodiscard]] bool parse(std::vector<uint8_t> bytes);
    void clear();

    [[nodiscard]] bool loaded() const { return !entries_.empty(); }
    [[nodiscard]] size_t node_count() const { return entries_.size(); }
    [[nodiscard]] bool decode_prepared_request(
        const WorldMapMovementRequestPreparation& request,
        size_t output_limit, std::vector<uint8_t>* out) const;

private:
    struct Entry {
        uint32_t offset = 0;
        uint32_t size = 0;
    };
    std::vector<uint8_t> bytes_;
    std::vector<Entry> entries_;
};

} // namespace awl
