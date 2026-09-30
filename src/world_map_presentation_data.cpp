#include "awl/world_map_presentation_data.h"

#include "awl/world_map_message_stream.h"

#include <limits>

namespace awl {
namespace {
using Status = WorldMapPresentationDataStatus;

Status read(const uint8_t* data, size_t size, size_t offset, WorldMapMessageToken* token) {
    if (offset >= size) return Status::MissingStopToken;
    const auto status = read_world_map_message_token(data, size, offset, token);
    if (status == WorldMapMessageStreamStatus::TruncatedToken) return Status::TruncatedToken;
    return status == WorldMapMessageStreamStatus::Decoded ? Status::Prepared : Status::InvalidInput;
}

Status validate_start(const uint8_t* data, size_t size, std::optional<size_t> start) {
    if (data == nullptr || size > std::numeric_limits<uint32_t>::max()) return Status::InvalidInput;
    if (!start) return Status::Prepared;
    if (*start >= size) return size == 0 && *start == 0 ? Status::MissingStopToken : Status::InvalidInput;
    size_t cursor = 0;
    while (cursor < *start) {
        WorldMapMessageToken token;
        const auto status = read(data, size, cursor, &token);
        if (status != Status::Prepared) return status;
        cursor += token.byte_count;
        if (token.terminator || cursor > *start) return Status::InvalidInput;
    }
    return Status::Prepared;
}

Status count_pass(const uint8_t* data, size_t size, std::optional<size_t> start,
                  WorldMapPresentationPass* out) {
    WorldMapPresentationPass pass;
    if (!start) { *out = pass; return Status::Prepared; }
    size_t cursor = *start;
    for (;;) {
        WorldMapMessageToken token;
        const auto status = read(data, size, cursor, &token);
        if (status != Status::Prepared) return status;
        // FUN_801047D4 keeps zero's pointer; FUN_801047E0 advances tag 1.
        if (token.tag == 0 || token.tag == 1) {
            pass.stop_offset = cursor;
            pass.next_offset = cursor + (token.tag == 1 ? token.byte_count : 0u);
            pass.stop = token.tag == 0 ? WorldMapPresentationStop::Terminator : WorldMapPresentationStop::Separator;
            *out = pass;
            return Status::Prepared;
        }
        if (token.tag == 2 || token.visitor_slot == 0xbc) {
            // FUN_80104814 delegates tag 2 to FUN_80104840 at +0xC0.
            // The limit branch does not write visitor +0x10.
            if (pass.units == 21) {
                pass.stop_offset = cursor;
                pass.stop = WorldMapPresentationStop::UnitLimit;
                *out = pass;
                return Status::Prepared;
            }
            ++pass.units;
        }
        cursor += token.byte_count;
    }
}

Status count_window(const uint8_t* data, size_t size, std::optional<size_t> start,
                    WorldMapPresentationWindow* out) {
    WorldMapPresentationWindow window;
    for (auto& pass : window.passes) {
        const auto status = count_pass(data, size, start, &pass);
        if (status != Status::Prepared) return status;
        window.units += pass.units;
        start = pass.next_offset;
    }
    *out = window;
    return Status::Prepared;
}
} // namespace

WorldMapPresentationDataStatus count_world_map_presentation_window(
    const uint8_t* data, size_t size, std::optional<size_t> start_offset,
    WorldMapPresentationWindow* out) {
    if (out == nullptr) return Status::InvalidInput;
    const auto status = validate_start(data, size, start_offset);
    if (status != Status::Prepared) return status;
    return count_window(data, size, start_offset, out);
}

WorldMapPresentationDataStatus resume_world_map_presentation_data(
    const uint8_t* data, size_t size, size_t start_offset, WorldMapPresentationResume* out) {
    if (out == nullptr) return Status::InvalidInput;
    auto status = validate_start(data, size, start_offset);
    if (status != Status::Prepared) return status;
    WorldMapPresentationResume resume;
    WorldMapMessageToken token;
    status = read(data, size, start_offset, &token);
    if (status != Status::Prepared) return status;
    resume.skipped_separator = token.tag == 1;
    resume.start_offset = start_offset + (resume.skipped_separator ? token.byte_count : 0u);
    status = count_window(data, size, resume.start_offset, &resume.window);
    if (status != Status::Prepared) return status;
    *out = resume;
    return Status::Prepared;
}

WorldMapPresentationDataStatus advance_world_map_presentation_data(
    const uint8_t* data, size_t size, std::optional<size_t> start_offset,
    uint32_t revealed_units, WorldMapPresentationAdvance* out) {
    if (out == nullptr) return Status::InvalidInput;
    auto status = validate_start(data, size, start_offset);
    if (status != Status::Prepared) return status;
    WorldMapPresentationPass dropped;
    status = count_pass(data, size, start_offset, &dropped);
    if (status != Status::Prepared) return status;
    WorldMapPresentationAdvance advance;
    advance.start_offset = dropped.next_offset;
    advance.dropped_units = dropped.units;
    advance.remaining_revealed_units = revealed_units - dropped.units;
    status = count_window(data, size, advance.start_offset, &advance.window);
    if (status != Status::Prepared) return status;
    *out = advance;
    return Status::Prepared;
}
} // namespace awl
