#include "awl/world_map_message_staging.h"

#include "awl/world_map_message_stream.h"

#include <utility>

namespace awl {
namespace {

using Status = WorldMapMessageStagingStatus;

bool requires_source(uint8_t tag) {
    // Complete overrides in counting vtable 802BCD30 / copying 802BCBA0.
    // Tag 0x29 is handled separately with supplied indexed message spans.
    return (tag >= 0x20 && tag <= 0x28) || (tag >= 0x2a && tag <= 0x2d) ||
        tag == 0x37 || tag == 0x38;
}

struct Staging {
    const WorldMapMessageStagingContext& context;
    uint32_t limit;
    WorldMapStagedMessage message;
    std::array<bool, 8> active_indices{};

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
                uint32_t index = source.data[token.offset + 1];
                if ((index & 0x80u) != 0) {
                    index &= 0x7fu;
                    index = index < context.argument_words.size() ? context.argument_words[index] : 0;
                }
                if (index >= context.indexed_messages.size()) return Status::InvalidContextIndex;
                if (active_indices[index]) return Status::CyclicSubstitution;
                active_indices[index] = true;
                ++message.context_expansions;
                const auto nested = walk(context.indexed_messages[index], false);
                active_indices[index] = false;
                if (nested != Status::Prepared) return nested;
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
