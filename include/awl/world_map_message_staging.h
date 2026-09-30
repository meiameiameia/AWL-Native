#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace awl {

struct WorldMapMessageSpan {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

struct WorldMapMessageStagingContext {
    // Supplied words corresponding to the staging owner's +0x44 array.
    std::array<uint32_t, 8> argument_words{};
    // Bounded native views corresponding to its +4 indexed message pointers.
    // Missing views stop expansion; original cache/global ownership is absent.
    std::array<WorldMapMessageSpan, 8> indexed_messages{};
};

struct WorldMapStagedMessage {
    std::vector<uint8_t> bytes;
    size_t consumed_bytes = 0; // Root input through its zero tag, not padding.
    uint64_t context_expansions = 0;
};

enum class WorldMapMessageStagingStatus {
    Prepared,
    RequiresSubstitution,
    InvalidInput,
    TruncatedToken,
    MissingTerminator,
    InvalidContextIndex,
    CyclicSubstitution,
    OutputLimitExceeded,
};

// Bounded FUN_800FBCC0/FUN_800FC438 visitor composition: ordinary tokens
// retain their bytes, tag 0x29 recursively expands a supplied indexed stream,
// and nested zero terminators are overwritten by subsequent outer tokens.
// High-bit tag-0x29 arguments use the supplied eight-word argument array;
// out-of-range argument references yield zero as in FUN_80188BBC. Resolved
// message indices outside 0..7, cycles, malformed streams, and output beyond
// byte_limit are native rejections. Other overridden staging token families
// require untranslated sources and cannot be copied as ordinary tokens.
// Failure preserves output. No original allocation, presentation, selection
// activation, source lookup, feedback, or manager result is accepted here.
[[nodiscard]] WorldMapMessageStagingStatus stage_world_map_message(
    const uint8_t* data, size_t size, const WorldMapMessageStagingContext& context,
    uint32_t byte_limit, WorldMapStagedMessage* out);

} // namespace awl
