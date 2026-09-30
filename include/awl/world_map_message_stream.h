#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace awl {

struct WorldMapMessageToken {
    size_t offset = 0;
    uint8_t tag = 0;
    uint8_t byte_count = 0;
    // Verified original visitor offset; retained as an opaque dispatch identity.
    uint8_t visitor_slot = 0;
    bool terminator = false;
};

enum class WorldMapMessageStreamStatus {
    Decoded,
    InvalidInput,
    TruncatedToken,
    MissingTerminator,
};

struct WorldMapMessageStream {
    std::vector<WorldMapMessageToken> tokens;
    // Includes the zero-tag terminator, excludes subsequent opaque padding.
    size_t consumed_bytes = 0;
};

// Bounded FUN_80186E28 dispatch / FUN_801874C8 length translation. Every byte
// family has a verified size, but no control payload, glyph, or effect is
// interpreted. Invalid/truncated input preserves output.
[[nodiscard]] WorldMapMessageStreamStatus read_world_map_message_token(
    const uint8_t* data, size_t size, size_t offset, WorldMapMessageToken* out);

// Structural scan through the first zero tag, respecting token lengths even
// when their argument bytes contain zero. This does not execute FUN_8018738C's
// visitor callbacks or their early-stop results. Failure preserves output.
[[nodiscard]] WorldMapMessageStreamStatus scan_world_map_message_stream(
    const uint8_t* data, size_t size, WorldMapMessageStream* out);

} // namespace awl
