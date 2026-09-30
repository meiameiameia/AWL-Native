#include "awl/world_map_action_asset.h"

#include "awl/filesystem.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

namespace awl {
namespace {

uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

bool in_range(size_t size, uint64_t offset, uint64_t length) {
    return offset <= size && length <= static_cast<uint64_t>(size) - offset;
}

} // namespace

bool decode_world_map_action_clz(
    const uint8_t* data, size_t size, size_t output_limit,
    std::vector<uint8_t>* out) {
    if (out == nullptr || data == nullptr || size < 16 ||
        std::memcmp(data, "CLZ\0", 4) != 0) {
        return false;
    }
    const uint32_t total = be32(data + 4);
    // The target entries have one block, zero stride, and a block output
    // count equal to the total. Other block layouts are not yet supported.
    if (total == 0 || total > output_limit || be32(data + 8) != 0 ||
        be32(data + 12) != total) {
        return false;
    }
    std::vector<uint8_t> decoded;
    decoded.reserve(total);
    size_t cursor = 16;
    uint32_t flags = 0;
    while (decoded.size() < total) {
        // FUN_80178450 consumes flag bits from low to high, with one
        // meaning a match and zero a literal. 0xFF00 tracks eight tokens.
        flags >>= 1;
        if ((flags & 0xff00u) == 0) {
            if (cursor == size) {
                return false;
            }
            flags = static_cast<uint32_t>(data[cursor++]) | 0xff00u;
        }
        if ((flags & 1u) == 0) {
            if (cursor == size) {
                return false;
            }
            decoded.push_back(data[cursor++]);
        } else {
            if (!in_range(size, cursor, 2)) {
                return false;
            }
            const uint32_t low = data[cursor++];
            const uint32_t high = data[cursor++];
            const size_t distance = 4096u - (low | ((high & 0xf0u) << 4));
            const size_t length = (high & 0x0fu) + 3u;
            if (distance > decoded.size() || length > total - decoded.size()) {
                return false;
            }
            // Byte-at-a-time copying preserves overlapping matches.
            for (size_t i = 0; i < length; ++i) {
                decoded.push_back(decoded[decoded.size() - distance]);
            }
        }
    }
    if (cursor != size) {
        return false;
    }
    out->swap(decoded);
    return true;
}

void WorldMapGlobalActionArchive::clear() {
    entries_.clear();
    bytes_.clear();
}

void WorldMapActionScript::clear() {
    bytes_.clear();
    code_offset_ = 0;
    code_count_ = 0;
    string_count_ = 0;
    options_ = 0;
}

bool WorldMapActionScript::parse(std::vector<uint8_t> bytes) {
    clear();
    if (bytes.size() < 12 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "SCR ", 4) != 0 ||
        be32(bytes.data() + 4) != bytes.size()) {
        return false;
    }
    bool saw_code = false;
    bool saw_strings = false;
    bool saw_options = false;
    size_t code_offset = 0;
    uint32_t code_count = 0;
    uint32_t string_count = 0;
    uint32_t options = 0;
    size_t cursor = 12;
    // This target format uses big-endian lengths and no RIFF padding.
    // The DOL's inclusive end test can read past the declared end; native
    // parsing instead requires complete chunks ending exactly at that bound.
    while (cursor < bytes.size()) {
        if (!in_range(bytes.size(), cursor, 8)) {
            return false;
        }
        const uint8_t* tag = bytes.data() + cursor;
        const uint32_t length = be32(tag + 4);
        cursor += 8;
        if (length < 4 || !in_range(bytes.size(), cursor, length)) {
            return false;
        }
        const uint32_t value = be32(bytes.data() + cursor);
        if (std::memcmp(tag, "CODE", 4) == 0) {
            if (saw_code || value == 0 ||
                4ull + static_cast<uint64_t>(value) * 8 != length) {
                return false;
            }
            saw_code = true;
            code_offset = cursor + 4;
            code_count = value;
        } else if (std::memcmp(tag, "STR ", 4) == 0) {
            if (saw_strings ||
                4ull + static_cast<uint64_t>(value) * 4 > length) {
                return false;
            }
            saw_strings = true;
            string_count = value;
            // The table/blob are owned but not dereferenced. Their offset
            // interpretation and runtime string use remain untranslated.
        } else if (std::memcmp(tag, "OPT ", 4) == 0) {
            if (saw_options || length != 4) {
                return false;
            }
            saw_options = true;
            options = value;
        } else {
            return false;
        }
        cursor += length;
    }
    if (!saw_code || !saw_options) {
        return false;
    }
    bytes_.swap(bytes);
    code_offset_ = code_offset;
    code_count_ = code_count;
    string_count_ = string_count;
    options_ = options;
    return true;
}

bool WorldMapActionScript::instruction(
    size_t index, WorldMapActionInstruction* out) const {
    if (out != nullptr) {
        *out = {};
    }
    if (!loaded() || out == nullptr || index >= code_count_) {
        return false;
    }
    const uint8_t* record = bytes_.data() + code_offset_ + index * 8;
    out->opcode = record[0];
    out->flags = record[1];
    out->reserved = static_cast<uint16_t>(
        (static_cast<uint16_t>(record[2]) << 8) | record[3]);
    out->operand = be32(record + 4);
    return true;
}

bool WorldMapActionScript::initialize_state(
    uint32_t mode_flags, WorldMapActionScriptState* out) const {
    if (!loaded() || out == nullptr) {
        return false;
    }
    *out = {};
    out->state_4 = 1;
    out->instruction_count_10 = code_count_;
    out->string_count_14 = string_count_;
    out->options_1b4 = options_;
    out->mode_flags_528 = mode_flags;
    return true;
}

WorldMapActionStepStatus WorldMapActionScript::step(
    WorldMapActionScriptState* state, WorldMapActionStep* out) const {
    using Status = WorldMapActionStepStatus;
    if (out != nullptr) {
        *out = {};
    }
    if (!loaded() || state == nullptr || out == nullptr ||
        state->instruction_count_10 != code_count_ ||
        state->string_count_14 != string_count_ || state->options_1b4 != options_ ||
        state->stack_depth_1b0 >= state->stack_20.size()) {
        return Status::InvalidState;
    }
    if (state->state_4 == 0) {
        return Status::NotRunning;
    }
    WorldMapActionInstruction record;
    if (state->state_4 != 1 ||
        !instruction(state->instruction_index_0c, &record)) {
        return Status::InvalidState;
    }
    const uint32_t operand = record.operand +
        ((record.flags & 1u) != 0 ? state->operand_base_4d8 : 0u);
    out->instruction_index = state->instruction_index_0c;
    out->effective_operand = operand;
    out->opcode = record.opcode;
    if (record.opcode == 0x25) {
        // The DOL calls virtual +0x0C after advancing PC. Native pauses before
        // consumption so a future game-owned callback can perform it once.
        return Status::RequiresCallback;
    }
    auto next = *state;
    ++next.instruction_index_0c;
    auto top = [&next]() -> uint32_t& {
        return next.stack_20[next.stack_depth_1b0];
    };
    auto pop = [&next, &top]() {
        const uint32_t value = top();
        if (next.stack_depth_1b0 != 0) {
            --next.stack_depth_1b0;
        }
        return value;
    };
    auto push = [&next](uint32_t value) {
        // Slot zero is the DOL's empty-stack sentinel. Its guard permits
        // depth 100, overlapping +0x1B0; native rejects that write.
        if (static_cast<size_t>(next.stack_depth_1b0) + 1 >= next.stack_20.size()) {
            return false;
        }
        next.stack_20[++next.stack_depth_1b0] = value;
        return true;
    };
    auto jump = [&next, this](uint32_t target) {
        if (target >= code_count_) {
            return false;
        }
        next.instruction_index_0c = target;
        return true;
    };
    auto signed_value = [](uint32_t value) -> int64_t {
        return (value & 0x80000000u) != 0
            ? static_cast<int64_t>(value) - 0x100000000ll : value;
    };
    switch (record.opcode) {
    case 0x00: // no-op
        break;
    case 0x01: // store through an index on the stack, retaining the value
    case 0x02: // add to that variable
    case 0x03: { // subtract from that variable
        const uint32_t value = pop();
        const uint32_t index = top();
        if (index >= next.variables_1b8.size()) {
            return Status::InvalidOperand;
        }
        uint32_t result = value;
        if (record.opcode == 0x02) {
            result = next.variables_1b8[index] + value;
        } else if (record.opcode == 0x03) {
            result = next.variables_1b8[index] - value;
        }
        next.variables_1b8[index] = result;
        top() = result;
        break;
    }
    case 0x07: {
        const uint32_t value = pop();
        top() += value;
        break;
    }
    case 0x08: {
        const uint32_t value = pop();
        top() -= value;
        break;
    }
    case 0x0c: {
        const uint32_t value = pop();
        top() = top() != 0 && value != 0 ? 1u : 0u;
        break;
    }
    case 0x0d: {
        const uint32_t value = pop();
        top() = top() != 0 || value != 0 ? 1u : 0u;
        break;
    }
    case 0x0e:
    case 0x0f: {
        // PowerPC slw uses the low six shift bits; bit five yields zero.
        const uint32_t shift = next.options_1b4 & 63u;
        const uint32_t unit = shift < 32 ? 1u << shift : 0u;
        top() = record.opcode == 0x0e ? top() + unit : top() - unit;
        break;
    }
    case 0x10:
        top() = 0u - top();
        break;
    case 0x11:
        top() = top() == 0 ? 1u : 0u;
        break;
    case 0x13:
        if (operand >= next.variables_1b8.size() ||
            !push(next.variables_1b8[operand])) {
            return Status::InvalidOperand;
        }
        break;
    case 0x14:
        if (operand >= next.variables_1b8.size()) {
            return Status::InvalidOperand;
        }
        next.variables_1b8[operand] = pop();
        break;
    case 0x15: {
        const uint32_t value = top();
        if (!push(value)) {
            return Status::InvalidOperand;
        }
        break;
    }
    case 0x16:
        (void)pop();
        break;
    case 0x17:
        if (!push(operand)) {
            return Status::InvalidOperand;
        }
        break;
    case 0x18:
        if (!jump(operand)) {
            return Status::InvalidOperand;
        }
        break;
    case 0x19:
    case 0x1a:
    case 0x1b:
    case 0x1c:
    case 0x1d:
    case 0x1e: {
        const int64_t value = signed_value(pop());
        const bool taken =
            (record.opcode == 0x19 && value < 0) ||
            (record.opcode == 0x1a && value <= 0) ||
            (record.opcode == 0x1b && value == 0) ||
            (record.opcode == 0x1c && value != 0) ||
            (record.opcode == 0x1d && value >= 0) ||
            (record.opcode == 0x1e && value > 0);
        if (taken && !jump(operand)) {
            return Status::InvalidOperand;
        }
        break;
    }
    case 0x1f:
        if (!push(next.instruction_index_0c) || !jump(operand)) {
            return Status::InvalidOperand;
        }
        break;
    case 0x20:
        if (!jump(pop())) {
            return Status::InvalidOperand;
        }
        break;
    case 0x21:
    case 0x22:
    case 0x23: {
        uint32_t base = operand;
        if (record.opcode == 0x22) {
            base = next.operand_base_4d8 + operand;
        } else if (record.opcode == 0x23) {
            base = next.operand_base_4d8 - operand;
        }
        if (base >= next.variables_1b8.size()) {
            return Status::InvalidOperand;
        }
        next.operand_base_4d8 = base;
        break;
    }
    case 0x24:
        next.state_4 = 0;
        *state = next;
        return Status::Halted;
    default:
        return Status::UnsupportedOpcode;
    }
    *state = next;
    return Status::Advanced;
}

WorldMapActionStepStatus WorldMapActionScript::step_world_map(
    WorldMapActionScriptState* state, WorldMapActionStep* out) const {
    using Status = WorldMapActionStepStatus;
    const Status status = step(state, out);
    if (status != Status::RequiresCallback ||
        (out->effective_operand != 0 && out->effective_operand != 66)) {
        return status;
    }
    auto next = *state;
    ++next.instruction_index_0c;
    if (out->effective_operand == 66) {
        // FUN_8010C474 reads the command's eight descriptor slots in reverse.
        // Command 66 has integer slot zero and seven zero slots. sraw scales
        // the signed word by options; its low six shift bits are significant.
        const uint32_t value = next.stack_20[next.stack_depth_1b0];
        const uint32_t shift = next.options_1b4 & 63u;
        const bool negative = (value & 0x80000000u) != 0;
        uint32_t argument = value;
        if (shift >= 32) {
            argument = negative ? UINT32_MAX : 0u;
        } else if (shift != 0) {
            argument = value >> shift;
            if (negative) {
                argument |= UINT32_MAX << (32u - shift);
            }
        }
        out->callback_arguments[0] = argument;
        if (next.stack_depth_1b0 != 0) {
            --next.stack_depth_1b0;
        }
    }
    *state = next;
    // Command 0 returns one from FUN_8010C604; FUN_801861B8 ends its pass.
    // Command 66 reaches the verified default return-zero branch.
    return out->effective_operand == 0 ? Status::Yielded : Status::Advanced;
}

WorldMapActionStepStatus WorldMapActionScript::step_world_map_request(
    WorldMapActionScriptState* state, WorldMapCommand4Snapshot* command4,
    WorldMapActionStep* out) const {
    using Status = WorldMapActionStepStatus;
    if (out != nullptr) *out = {};
    if (state == nullptr || command4 == nullptr || out == nullptr) {
        return Status::InvalidState;
    }
    // Inspect a copy so even ordinary instructions are not consumed by this
    // command-specific entry point. Common validation and metadata are reused.
    auto inspected = *state;
    const Status status = step(&inspected, out);
    if (status != Status::RequiresCallback) {
        return status == Status::Advanced || status == Status::Halted
            ? Status::InvalidOperand : status;
    }
    if (out->effective_operand != 4) return Status::RequiresCallback;

    WorldMapCommandPreparation prepared;
    const auto preparation = prepare_world_map_command(*state, command4, &prepared);
    if (preparation == WorldMapCommandPreparationStatus::RequiresManagerSnapshot) {
        return Status::RequiresCallback;
    }
    if (preparation != WorldMapCommandPreparationStatus::Prepared) {
        return Status::InvalidState;
    }
    *out = prepared.instruction;
    uint32_t result = prepared.stack_result;
    if (prepared.effect == WorldMapCommandEffect::BeginRequest4) {
        if (!command4->has_manager_state || command4->manager_state_48 == 0) {
            // FUN_801020E4's idle branch needs resource/presentation setup.
            // Do not accept it, store keys, or pop/push until supported.
            return Status::RequiresCallback;
        }
        // FUN_801020E4 rejects nonzero +0x48 before any manager write.
        // The dispatcher already stored both keys and pushes -1 on failure.
        result = UINT32_MAX;
    }
    auto next = prepared.after_arguments;
    if (static_cast<size_t>(next.stack_depth_1b0) + 1 >= next.stack_20.size()) {
        return Status::InvalidOperand;
    }
    // FUN_80112D08: PowerPC slw uses the low six option bits and produces
    // zero when bit five is set. Unsigned arithmetic preserves word wrap.
    const uint32_t shift = next.options_1b4 & 63u;
    next.stack_20[++next.stack_depth_1b0] = shift < 32 ? result << shift : 0u;
    *state = next;
    command4->key_530 = prepared.next_key_530;
    command4->key_534 = prepared.next_key_534;
    return Status::Advanced;
}

WorldMapCommandPreparationStatus WorldMapActionScript::prepare_world_map_command(
    const WorldMapActionScriptState& state,
    const WorldMapCommand4Snapshot* command4,
    WorldMapCommandPreparation* out) const {
    using Status = WorldMapCommandPreparationStatus;
    if (out == nullptr) {
        return Status::InvalidState;
    }
    *out = {};
    WorldMapCommandPreparation prepared;
    prepared.after_arguments = state;
    if (step(&prepared.after_arguments, &prepared.instruction) !=
        WorldMapActionStepStatus::RequiresCallback) {
        return Status::InvalidState;
    }
    const uint32_t command = prepared.instruction.effective_operand;
    if (command != 4 && command != 65) {
        return Status::UnsupportedCommand;
    }
    if (command == 4 && command4 == nullptr) {
        return Status::InvalidState;
    }
    auto signed_word = [](uint32_t value) -> int64_t {
        return (value & 0x80000000u) != 0
            ? static_cast<int64_t>(value) - 0x100000000ll : value;
    };
    const uint32_t shift = state.options_1b4 & 63u;
    float float_argument = 0;
    // Both profiles have three active slots. FUN_8010C474 visits them
    // backwards, including sentinel reads when depth has reached zero.
    for (size_t slot = 3; slot-- > 0;) {
        auto& next = prepared.after_arguments;
        const uint32_t value = next.stack_20[next.stack_depth_1b0];
        uint32_t argument = value;
        if (command == 65 && slot == 2) {
            // DOL conversion rounds numerator and denominator to float before
            // fdivs. A zero slw denominator is rejected by the native boundary.
            if (shift >= 32) {
                return Status::InvalidOperand;
            }
            const float numerator = static_cast<float>(signed_word(value));
            const float denominator = static_cast<float>(signed_word(1u << shift));
            float_argument = numerator / denominator;
            std::memcpy(&argument, &float_argument, sizeof(argument));
        } else if (shift >= 32) {
            argument = (value & 0x80000000u) != 0 ? UINT32_MAX : 0u;
        } else if (shift != 0) {
            argument = value >> shift;
            if ((value & 0x80000000u) != 0) {
                argument |= UINT32_MAX << (32u - shift);
            }
        }
        prepared.instruction.callback_arguments[slot] = argument;
        if (next.stack_depth_1b0 != 0) {
            --next.stack_depth_1b0;
        }
    }
    ++prepared.after_arguments.instruction_index_0c;
    const auto& arguments = prepared.instruction.callback_arguments;
    if (command == 4) {
        prepared.next_key_530 = command4->key_530;
        prepared.next_key_534 = command4->key_534;
        if (command4->key_530 != arguments[0] || command4->key_534 != arguments[1]) {
            prepared.effect = WorldMapCommandEffect::BeginRequest4;
            prepared.next_key_530 = arguments[0];
            prepared.next_key_534 = arguments[1];
            // The result depends on FUN_80113864 -> FUN_801020E4 acceptance.
        } else {
            if (!command4->has_manager_state) {
                return Status::RequiresManagerSnapshot;
            }
            prepared.has_stack_result = true;
            if (command4->manager_state_48 != 0) {
                prepared.effect = WorldMapCommandEffect::PollRequest4;
                prepared.stack_result = 0;
            } else {
                prepared.effect = WorldMapCommandEffect::CompleteRequest4;
                prepared.next_key_530 = UINT32_MAX;
                prepared.next_key_534 = UINT32_MAX;
                prepared.stack_result = command4->manager_result_1c0 == UINT32_MAX
                    ? UINT32_MAX : command4->manager_result_1c0 + 1u;
            }
        }
    } else {
        int64_t level = signed_word(arguments[1]);
        if (level > 127) level = 127;
        // Match the original's upper level clamp only; negative values are
        // retained, including their later byte conversion on the indexed path.
        float milliseconds = 1000.0f * float_argument;
        if (milliseconds > 65535.0f) milliseconds = 65535.0f;
        if (milliseconds < 0.0f) milliseconds = 0.0f;
        const int64_t target = signed_word(arguments[0]);
        if (target >= 0 && target < 2) {
            prepared.effect = WorldMapCommandEffect::LevelTransition65;
            prepared.normalized_level = static_cast<float>(level) / 127.0f;
            prepared.transition_seconds = milliseconds / 1000.0f;
        } else {
            prepared.effect = WorldMapCommandEffect::IndexedLevel65;
            prepared.indexed_target = static_cast<uint8_t>(arguments[0]);
            prepared.indexed_level = static_cast<uint8_t>(level);
            prepared.transition_milliseconds = static_cast<uint32_t>(milliseconds);
        }
    }
    *out = prepared;
    return Status::Prepared;
}

bool WorldMapGlobalActionArchive::parse(std::vector<uint8_t> bytes) {
    clear();
    if (bytes.size() < 0x20 || be32(bytes.data()) != 0x55aa382du) {
        return false;
    }
    const uint32_t nodes = be32(bytes.data() + 4);
    const uint32_t metadata_size = be32(bytes.data() + 8);
    const uint32_t data_start = be32(bytes.data() + 12);
    if (nodes < 0x20 || !in_range(bytes.size(), nodes, metadata_size) ||
        static_cast<uint64_t>(nodes) + metadata_size > data_start ||
        !in_range(bytes.size(), nodes, 12)) {
        return false;
    }
    const uint32_t count = be32(bytes.data() + nodes + 8);
    const uint64_t names = static_cast<uint64_t>(nodes) +
                           static_cast<uint64_t>(count) * 12;
    const uint64_t names_end = static_cast<uint64_t>(nodes) + metadata_size;
    if (count < 2 || names >= names_end || data_start > bytes.size() ||
        !in_range(bytes.size(), nodes, static_cast<uint64_t>(count) * 12) ||
        be32(bytes.data() + nodes) != 0x01000000u ||
        be32(bytes.data() + nodes + 4) != 0) {
        return false;
    }
    std::vector<Entry> entries(count);
    std::vector<std::pair<uint64_t, uint64_t>> extents;
    for (uint32_t index = 1; index < count; ++index) {
        const size_t node = nodes + static_cast<size_t>(index) * 12;
        const uint32_t tag = be32(bytes.data() + node);
        const uint32_t offset = be32(bytes.data() + node + 4);
        const uint32_t size = be32(bytes.data() + node + 8);
        const uint64_t name = names + (tag & 0x00ffffffu);
        // Common.arc is flat. Reject directories and unknown node types.
        if ((tag >> 24) != 0 || name >= names_end || offset < data_start ||
            size == 0 || !in_range(bytes.size(), offset, size)) {
            return false;
        }
        const void* end = std::memchr(bytes.data() + name, 0,
                                     static_cast<size_t>(names_end - name));
        if (end == nullptr || end == bytes.data() + name) {
            return false;
        }
        entries[index] = {offset, size};
        extents.emplace_back(offset, static_cast<uint64_t>(offset) + size);
    }
    std::sort(extents.begin(), extents.end());
    for (size_t i = 1; i < extents.size(); ++i) {
        if (extents[i].first < extents[i - 1].second) {
            return false;
        }
    }
    bytes_.swap(bytes);
    entries_.swap(entries);
    return true;
}

bool WorldMapGlobalActionArchive::load() {
    clear();
    void* raw = nullptr;
    size_t size = 0;
    if (!filesystem_read_entire_file("/files/Common.arc", &raw, &size)) {
        return false;
    }
    std::unique_ptr<void, decltype(&filesystem_free_file_data)> owned(
        raw, &filesystem_free_file_data);
    const auto* first = static_cast<const uint8_t*>(raw);
    return first != nullptr && parse(std::vector<uint8_t>(first, first + size));
}

bool WorldMapGlobalActionArchive::decode_prepared_request(
    const WorldMapMovementRequestPreparation& request,
    size_t output_limit, std::vector<uint8_t>* out) const {
    if (!loaded() || !request.use_global_action_list ||
        request.action_index < 300u || request.action_index >= 0x3ffu ||
        request.decoder_flag > 1 || request.action_list_index <= 0 ||
        request.action_list_index != static_cast<int32_t>(request.action_index - 300u) ||
        static_cast<size_t>(request.action_list_index) >= entries_.size()) {
        return false;
    }
    const Entry& entry = entries_[static_cast<size_t>(request.action_list_index)];
    return decode_world_map_action_clz(bytes_.data() + entry.offset,
                                      entry.size, output_limit, out);
}

} // namespace awl
