#include "awl/world_map_message_staging.h"

#include "awl/world_map_message_stream.h"

#include <utility>

namespace awl {
namespace {

using Status = WorldMapMessageStagingStatus;

bool requires_source(uint8_t tag) {
    // Complete overrides in counting vtable 802BCD30 / copying 802BCBA0.
    // Indexed message and numeric replacements are handled separately.
    return (tag >= 0x20 && tag <= 0x28) || (tag >= 0x2b && tag <= 0x2d) || tag == 0x38;
}

struct EncodedNumber {
    std::array<uint8_t, 22> bytes{}; // Ten decimal digits and a sign, two bytes each.
    size_t size = 0;
};

EncodedNumber encode_number(uint32_t word) {
    EncodedNumber number;
    const bool negative = (word & 0x80000000u) != 0;
    // FUN_802373A8 takes the magnitude as a wrapped word, including INT32_MIN.
    uint32_t magnitude = negative ? 0u - word : word;
    if (negative) {
        number.bytes[number.size++] = 0x80;
        number.bytes[number.size++] = 0x0a;
    }
    std::array<uint8_t, 10> reversed{};
    size_t digits = 0;
    do {
        reversed[digits++] = static_cast<uint8_t>(magnitude % 10u);
        magnitude /= 10u;
    } while (magnitude != 0);
    // FUN_800FD670's digit table at 80251A24 is verified as pairs (80, digit).
    while (digits != 0) {
        number.bytes[number.size++] = 0x80;
        number.bytes[number.size++] = reversed[--digits];
    }
    return number;
}

struct Staging {
    const WorldMapMessageStagingContext& context;
    uint32_t limit;
    WorldMapStagedMessage message;
    std::array<bool, 8> active_indices{};

    uint32_t resolve_argument(uint32_t word, uint32_t reference_bit) const {
        if ((word & reference_bit) == 0) return word;
        const uint32_t index = word & ~reference_bit;
        return index < context.argument_words.size() ? context.argument_words[index] : 0;
    }

    Status append_number(uint32_t word, uint32_t minimum_width, bool wrapped) {
        // FUN_800FD11C recursively multiplies its divisor as a wrapped word
        // once the magnitude reaches 10^9. Its size can then disagree with
        // FUN_800FD670's digit output; do not invent allocation-gap bytes.
        const uint32_t magnitude = (word & 0x80000000u) != 0 ? 0u - word : word;
        if (magnitude >= 1000000000u) return Status::UnsupportedNumericValue;
        const auto number = encode_number(word);
        // FUN_800FC174 pads with tag 2 before the number, using half the
        // non-terminator byte count (digits plus any negative sign).
        const uint64_t units = number.size / 2;
        const uint64_t padding = wrapped ? number.size % 2 :
            (minimum_width > units ? minimum_width - units : 0);
        const uint64_t required = padding + number.size + (wrapped ? 5u : 0u);
        if (required > limit - message.bytes.size()) return Status::OutputLimitExceeded;
        // FUN_800FC30C preserves 18,2,2; parity padding; number; 2,19.
        if (wrapped) message.bytes.insert(message.bytes.end(), {0x18, 2, 2});
        message.bytes.insert(message.bytes.end(), static_cast<size_t>(padding), 2);
        message.bytes.insert(message.bytes.end(), number.bytes.begin(), number.bytes.begin() + number.size);
        if (wrapped) message.bytes.insert(message.bytes.end(), {2, 0x19});
        ++message.numeric_expansions;
        return Status::Prepared;
    }

    Status walk(WorldMapMessageSpan source, bool root) {
        if (source.data == nullptr) return source.size == 0 ? Status::RequiresSubstitution : Status::InvalidInput;
        size_t offset = 0;
        while (offset < source.size) {
            WorldMapMessageToken token;
            const auto read = read_world_map_message_token(source.data, source.size, offset, &token);
            if (read == WorldMapMessageStreamStatus::TruncatedToken) return Status::TruncatedToken;
            if (read != WorldMapMessageStreamStatus::Decoded) return Status::InvalidInput;
            offset += token.byte_count;
            if (requires_source(token.tag)) return Status::RequiresSubstitution;
            if (token.tag == 0x29) {
                // FUN_80187AF4 -> FUN_80188BBC, then FUN_800FB9D8 /
                // FUN_800FC120 select owner +4 + resolved_index * 4.
                const uint32_t index = resolve_argument(source.data[token.offset + 1], 0x80u);
                if (index >= context.indexed_messages.size()) return Status::InvalidContextIndex;
                if (active_indices[index]) return Status::CyclicSubstitution;
                active_indices[index] = true;
                ++message.context_expansions;
                const auto nested = walk(context.indexed_messages[index], false);
                active_indices[index] = false;
                if (nested != Status::Prepared) return nested;
            } else if (token.tag == 0x2a) {
                // FUN_80187B14 / FUN_80187920 decode byte arguments at +1/+2.
                const uint32_t index = resolve_argument(source.data[token.offset + 1], 0x80u);
                if (index >= context.numeric_words.size()) return Status::InvalidContextIndex;
                const uint32_t width = resolve_argument(source.data[token.offset + 2], 0x80u);
                const auto numeric = append_number(context.numeric_words[index], width, false);
                if (numeric != Status::Prepared) return numeric;
            } else if (token.tag == 0x37) {
                // FUN_80187C54 -> FUN_80188C00 reads a BE word, with its top
                // bit selecting the same supplied argument array.
                const auto* p = source.data + token.offset + 1;
                const uint32_t word = (static_cast<uint32_t>(p[0]) << 24) |
                    (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
                const auto numeric = append_number(resolve_argument(word, 0x80000000u), 0, true);
                if (numeric != Status::Prepared) return numeric;
            } else {
                // Counting includes each terminator; nested callers subtract
                // one. Copying writes zero without advancing its cursor at
                // tag 0 (FUN_800FBE20), so subsequent outer bytes overwrite it.
                const size_t copied = token.terminator && !root ? 0 : token.byte_count;
                if (copied > limit - message.bytes.size()) return Status::OutputLimitExceeded;
                message.bytes.insert(message.bytes.end(), source.data + token.offset,
                                     source.data + token.offset + copied);
            }
            if (token.terminator) {
                if (root) message.consumed_bytes = offset;
                return Status::Prepared;
            }
        }
        return Status::MissingTerminator;
    }
};

} // namespace

WorldMapMessageStagingStatus stage_world_map_message(
    const uint8_t* data, size_t size, const WorldMapMessageStagingContext& context,
    uint32_t byte_limit, WorldMapStagedMessage* out) {
    if (data == nullptr || out == nullptr || byte_limit == 0) return Status::InvalidInput;
    Staging staging{context, byte_limit, {}, {}};
    const auto status = staging.walk({data, size}, true);
    if (status != Status::Prepared) return status;
    *out = std::move(staging.message);
    return Status::Prepared;
}

} // namespace awl
