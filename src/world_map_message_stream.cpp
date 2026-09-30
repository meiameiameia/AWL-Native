#include "awl/world_map_message_stream.h"

#include <utility>

namespace awl {
namespace {

uint8_t visitor_slot(uint8_t tag) {
    // Derived dispatch relationships, not original pointers/member tables.
    // FUN_80186E28 also sends unlisted tags through the default byte family.
    switch (tag) {
        case 0x00: return 0x0c;
        case 0x01: return 0x10;
        case 0x02: return 0x14;
        case 0x10: return 0x28;
        case 0x11: return 0x18;
        case 0x12: return 0x24;
        case 0x13: return 0x20;
        case 0x14: return 0x1c;
        case 0x15: return 0x2c;
        case 0x16: return 0x30;
        case 0x17: return 0x34;
        case 0x18: return 0x38;
        case 0x19: return 0x3c;
        case 0x20: return 0x50;
        case 0x21: return 0x48;
        case 0x22: return 0x58;
        case 0x23: return 0x54;
        case 0x24: return 0x4c;
        case 0x25: return 0x44;
        case 0x26: return 0x40;
        case 0x27: return 0x70;
        case 0x28: return 0x74;
        case 0x29: return 0x5c;
        case 0x2a: return 0x60;
        case 0x2b: return 0x64;
        case 0x2c: return 0x68;
        case 0x2d: return 0x6c;
        case 0x30: return 0x78;
        case 0x31: return 0x7c;
        case 0x32: return 0x88;
        case 0x33: return 0x84;
        case 0x34: return 0x80;
        case 0x35: return 0x8c;
        case 0x36: return 0x90;
        case 0x37: return 0x94;
        case 0x38: return 0x98;
        case 0x39: return 0x9c;
        case 0x40: return 0xa4;
        case 0x41: return 0xa0;
        case 0x50: return 0xa8;
        case 0x51: return 0xac;
        case 0x52: return 0xb0;
        case 0x53: return 0xb4;
        case 0x54: return 0xb8;
        default: return (tag & 0xf0u) == 0x80u ? 0xbc : 0xc0;
    }
}

uint8_t token_size(uint8_t slot) {
    // FUN_80187D44..80187F68: the selected length visitor writes a constant.
    switch (slot) {
        case 0x30: case 0x94: case 0xb0: return 5;
        case 0xa8: case 0xac: return 4;
        case 0x40: case 0x44: case 0x60: case 0x68: case 0x70:
        case 0x74: case 0x80: case 0x84: case 0x88: case 0xa0: return 3;
        case 0x38: case 0x48: case 0x50: case 0x5c: case 0x64:
        case 0x8c: case 0x90: case 0x98: case 0xa4: case 0xbc: return 2;
        default: return 1;
    }
}

} // namespace

WorldMapMessageStreamStatus read_world_map_message_token(
    const uint8_t* data, size_t size, size_t offset, WorldMapMessageToken* out) {
    using Status = WorldMapMessageStreamStatus;
    if (data == nullptr || out == nullptr || offset >= size) return Status::InvalidInput;
    const uint8_t tag = data[offset];
    const uint8_t slot = visitor_slot(tag);
    const uint8_t length = token_size(slot);
    if (length > size - offset) return Status::TruncatedToken;
    *out = {offset, tag, length, slot, tag == 0};
    return Status::Decoded;
}

WorldMapMessageStreamStatus scan_world_map_message_stream(
    const uint8_t* data, size_t size, WorldMapMessageStream* out) {
    using Status = WorldMapMessageStreamStatus;
    if (data == nullptr || out == nullptr) return Status::InvalidInput;
    WorldMapMessageStream stream;
    size_t offset = 0;
    while (offset < size) {
        WorldMapMessageToken token;
        const auto status = read_world_map_message_token(data, size, offset, &token);
        if (status != Status::Decoded) return status;
        stream.tokens.push_back(token);
        offset += token.byte_count;
        if (token.terminator) {
            stream.consumed_bytes = offset;
            *out = std::move(stream);
            return Status::Decoded;
        }
    }
    return Status::MissingTerminator;
}

} // namespace awl
