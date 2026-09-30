#include "awl/world_map_feedback.h"
#include "awl/world_map_presentation.h"

#include <cstdio>

namespace {
int failures = 0;
using Registry = awl::WorldMapFeedbackRegistry;
using Status = awl::WorldMapFeedbackStatus;
using Operation = awl::WorldMapFeedbackOperation;

void expect(bool condition, const char* message) {
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}

uint32_t result(const Registry& registry, uint64_t identity) {
    awl::WorldMapFeedbackRequest request;
    expect(registry.request(identity, &request), "owned request exists");
    return request.result_18;
}

uint32_t destination(const Registry& registry, uint64_t identity) {
    uint32_t word = 0;
    expect(registry.result_destination_word(identity, &word), "owned result destination exists");
    return word;
}

uint64_t find(const Registry& registry, uint64_t owner, uint32_t id, std::optional<uint64_t> after = std::nullopt) {
    uint64_t identity = 0;
    expect(registry.find_request(owner, id, after, &identity), "matching linked request exists");
    return identity;
}

void test_ownership() {
    Registry registry;
    expect(registry.add_result_destination(9, 42) && registry.create_request(1, 0x10006, 9) &&
        registry.create_request(2, 6) && registry.create_request(3, 6), "native requests own low-halfword IDs and result bindings");
    expect(result(registry, 1) == UINT32_MAX && destination(registry, 9) == 42,
        "constructor initializes self result but does not overwrite external destination");
    expect(registry.link_request(1, 10) && registry.link_request(2, 10) && registry.link_request(3, 10) &&
        registry.size(10) == 3 && find(registry, 10, 6) == 1 && find(registry, 10, 0x80000006, 1) == 2 &&
        find(registry, 10, 6, 2) == 3, "registration appends; query truncates ID and visits duplicate IDs in list order");
    uint64_t untouched = 99;
    expect(!registry.find_request(10, 6, 3, &untouched) && untouched == 99 &&
        !registry.find_request(10, 7, std::nullopt, &untouched) && untouched == 99,
        "lookup misses and final cursor preserve output");
    expect(registry.link_request(1, 10) && find(registry, 10, 6) == 2 && find(registry, 10, 6, 3) == 1 &&
        registry.size(10) == 3, "relink detaches and appends existing identity without duplicating it");
    expect(registry.link_request(2, 20) && registry.size(10) == 2 && registry.size(20) == 1 &&
        find(registry, 10, 6) == 3 && find(registry, 20, 6) == 2 &&
        !registry.find_request(10, 6, 2, &untouched) && untouched == 99,
        "transfer preserves both owner orders and rejects a foreign cursor");
    expect(registry.unlink_request(3) && !registry.unlink_request(3) && registry.size(10) == 1 &&
        result(registry, 3) == UINT32_MAX && find(registry, 10, 6) == 1,
        "unlink preserves owned request state");
    awl::WorldMapFeedbackStep step;
    expect(registry.start_request(1, {255, 255, 0}, 77, &step) == Status::Applied &&
        registry.link_request(1, 20) && result(registry, 1) == 77 && destination(registry, 9) == 77,
        "relink preserves started request and result binding");
    registry.clear_owner(20);
    expect(registry.size(20) == 0 && result(registry, 1) == 77 && result(registry, 2) == UINT32_MAX &&
        destination(registry, 9) == 77, "clear owner unlinks all without destroying or resetting their results");
    expect(registry.destroy_request(1) && destination(registry, 9) == UINT32_MAX && !registry.destroy_request(1),
        "destruction resets external result, even after unlink, without invoking playback stop");
    awl::WorldMapFeedbackRequest saved;
    saved.identity = 99; saved.result_18 = 55;
    expect(!registry.request(1, &saved) && saved.identity == 99 && saved.result_18 == 55,
        "destroyed request cannot return stale state");
    expect(!registry.create_request(0, 6) && !registry.create_request(2, 3) &&
        !registry.create_request(4, 6, 99) && !registry.create_request(4, 6, 0) &&
        !registry.add_result_destination(0, 2) && !registry.add_result_destination(9, 2) &&
        !registry.link_request(99, 10) && !registry.link_request(2, 0) &&
        !registry.find_request(0, 6, std::nullopt, &untouched) &&
        !registry.find_request(10, 6, 99, &untouched) && !registry.find_request(10, 6, std::nullopt, nullptr) &&
        !registry.request(2, nullptr) && !registry.result_destination_word(9, nullptr),
        "invalid or duplicate identities, unknown bindings, cursors and outputs are rejected");
    uint32_t missing = 88;
    expect(!registry.result_destination_word(99, &missing) && missing == 88 && destination(registry, 9) == UINT32_MAX,
        "failed destination access/duplicate insertion preserve owned word");
}

void test_backend_boundaries() {
    Registry registry;
    expect(registry.add_result_destination(9, 42) && registry.create_request(1, 6, 9) && registry.create_request(2, 3, 9),
        "two feedback requests can bind the same result destination");
    awl::WorldMapFeedbackStep step;
    const std::array<uint32_t, 3> arguments{0x12345678, 0xabcdef01, 0xffffffff};
    for (unsigned retry = 0; retry < 2; ++retry) {
        expect(registry.start_request(1, arguments, std::nullopt, &step) == Status::RequiresPlayback &&
            step.operation == Operation::Start && step.feedback_id == 6 && step.arguments == arguments &&
            result(registry, 1) == UINT32_MAX && destination(registry, 9) == 42,
            "absent start backend preserves both result locations across retries and forwards every argument word");
    }
    expect(registry.stop_request(1, std::nullopt, &step) == Status::Unchanged && step.return_word == 1 &&
        step.operation == Operation::None && destination(registry, 9) == 42,
        "unstarted stop returns one without backend or destination reset");
    expect(registry.start_request(1, arguments, UINT32_MAX, &step) == Status::Applied && step.return_word == 0 &&
        result(registry, 1) == UINT32_MAX && destination(registry, 9) == UINT32_MAX,
        "supplied start failure writes minus one to both results and permits a later retry");
    expect(registry.start_request(1, arguments, 0, &step) == Status::Applied && step.return_word == 1 &&
        result(registry, 1) == 0 && destination(registry, 9) == 0,
        "handle zero is a supported start result, not a failure");
    expect(registry.start_request(2, arguments, 77, &step) == Status::Applied && destination(registry, 9) == 77 &&
        registry.start_request(1, arguments, 88, &step) == Status::Unchanged && step.return_word == 0 &&
        step.operation == Operation::None && result(registry, 1) == 0 && destination(registry, 9) == 77,
        "already-started guard ignores another supplied result and preserves shared destination written by another request");
    expect(registry.stop_request(1, std::nullopt, &step) == Status::RequiresPlayback &&
        step.operation == Operation::Stop && step.playback_handle == 0 && result(registry, 1) == 0 && destination(registry, 9) == 77,
        "started stop requires a backend with the original handle");
    expect(registry.stop_request(1, 0x100, &step) == Status::Applied && step.return_word == 0x100 &&
        result(registry, 1) == 0 && destination(registry, 9) == 77,
        "stop preserves raw return but a zero low byte does not reset results");
    expect(registry.stop_request(1, 0x101, &step) == Status::Applied && step.return_word == 0x101 &&
        result(registry, 1) == UINT32_MAX && destination(registry, 9) == UINT32_MAX,
        "nonzero low byte resets self and shared external result without normalizing return");
    expect(registry.destroy_request(2) && destination(registry, 9) == UINT32_MAX,
        "destructor resets result binding without a made-up backend stop");
    const auto prior = step;
    expect(registry.start_request(99, arguments, std::nullopt, &step) == Status::InvalidInput &&
        registry.stop_request(99, std::nullopt, &step) == Status::InvalidInput &&
        registry.start_request(1, arguments, 77, nullptr) == Status::InvalidInput &&
        registry.stop_request(1, 1, nullptr) == Status::InvalidInput &&
        step.operation == prior.operation && step.return_word == prior.return_word &&
        step.request_identity == prior.request_identity && result(registry, 1) == UINT32_MAX,
        "invalid operations preserve request and previous operation output");
}

void hash_word(uint64_t& digest, uint32_t word) {
    for (unsigned shift : {24u, 16u, 8u, 0u}) digest = (digest ^ static_cast<uint8_t>(word >> shift)) * 1099511628211ull;
}

void test_result_matrix() {
    uint64_t digest = 14695981039346656037ull;
    unsigned cases = 0;
    for (uint32_t id : {0u, 1u, 3u, 6u, 0xffffu, 0x10000u, 0x10006u, 0x7fff8001u, 0x80000006u, UINT32_MAX}) {
        for (uint32_t external : {0u, 1u}) {
            for (uint32_t start : {0u, 1u, 127u, 128u, 255u, 256u, 0x80000000u, UINT32_MAX}) {
                for (uint32_t stop : {0u, 1u, 128u, 255u, 256u, 257u, 0x80000000u, UINT32_MAX}) {
                    Registry registry;
                    expect(registry.add_result_destination(9, 0x12345678) &&
                        registry.create_request(1, id, external ? std::optional<uint64_t>{9} : std::nullopt),
                        "matrix request initialization is valid");
                    awl::WorldMapFeedbackStep started, stopped;
                    const auto start_status = registry.start_request(1, {255, 255, 0}, start, &started);
                    const auto start_word = result(registry, 1);
                    const auto stop_status = registry.stop_request(1, stop, &stopped);
                    for (uint32_t value : {id, external, start, stop, static_cast<uint32_t>(started.feedback_id),
                        static_cast<uint32_t>(start_status), started.return_word, start_word,
                        static_cast<uint32_t>(stop_status), static_cast<uint32_t>(stopped.operation), stopped.return_word,
                        result(registry, 1), destination(registry, 9)}) hash_word(digest, value);
                    ++cases;
                }
            }
        }
    }
    expect(cases == 1280 && digest == 0xba987aedd6606b31ull,
        "ID truncation, start carry-return and stop low-byte/result writes match independent mapped-instruction matrix");
}

void test_presentation_composition() {
    const uint8_t bytes[] = {2, 0x30, 0}; // Invented protocol fixture.
    awl::WorldMapPresentationState presentation;
    presentation.display_offset_1c = presentation.resume_offset_20 = 0;
    presentation.window_units_28 = 1;
    presentation.phase = awl::WorldMapPresentationPhase::InputWait;
    awl::WorldMapPresentationStep plan;
    expect(awl::advance_world_map_presentation(bytes, sizeof(bytes), &presentation, 1, 0x100, 0, &plan) ==
        awl::WorldMapPresentationStatus::RequiresFeedback && plan.feedback_id == 3,
        "matching presentation input requests feedback before phase change");
    Registry registry;
    expect(plan.feedback_id && registry.create_request(1, *plan.feedback_id) && registry.link_request(1, 10),
        "presentation's planned feedback can have native request/list ownership");
    awl::WorldMapFeedbackStep feedback;
    expect(registry.start_request(1, {255, 255, 0}, std::nullopt, &feedback) == Status::RequiresPlayback &&
        result(registry, 1) == UINT32_MAX && presentation.phase == awl::WorldMapPresentationPhase::InputWait,
        "native request ownership cannot acknowledge playback or consume a presentation wait");
    presentation.phase = awl::WorldMapPresentationPhase::Reading;
    expect(awl::advance_world_map_presentation(bytes, sizeof(bytes), &presentation, 1, 0, 0, &plan) ==
        awl::WorldMapPresentationStatus::RequiresFeedback && plan.feedback_id == 6 && plan.feedback_channel_check,
        "ordinary glyph requires ordered query/conditional stop before new feedback start");
    expect(registry.create_request(2, 6) && registry.link_request(2, 10) && find(registry, 10, 6) == 2 &&
        registry.stop_request(2, std::nullopt, &feedback) == Status::Unchanged && feedback.return_word == 1 &&
        registry.create_request(3, 6) && registry.link_request(3, 10) &&
        registry.start_request(3, {255, 255, 0}, std::nullopt, &feedback) == Status::RequiresPlayback &&
        find(registry, 10, 6) == 2 && find(registry, 10, 6, 2) == 3 && presentation.revealed_units_24 == 0,
        "unstarted old request is not erased/reordered by stop; new append still blocks at playback");
}
} // namespace

int main() {
    test_ownership();
    test_backend_boundaries();
    test_result_matrix();
    test_presentation_composition();
    return failures == 0 ? 0 : 1;
}
