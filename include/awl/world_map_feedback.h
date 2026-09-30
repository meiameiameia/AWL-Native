#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace awl {

struct WorldMapFeedbackRequest {
    uint64_t identity = 0;
    uint16_t id_14 = 0;
    uint32_t result_18 = UINT32_MAX;
    // Native owned result-word identity; absent means result_18 itself.
    std::optional<uint64_t> result_destination;
};

enum class WorldMapFeedbackOperation { None, Start, Stop };
enum class WorldMapFeedbackStatus { Applied, Unchanged, RequiresPlayback, InvalidInput };

struct WorldMapFeedbackStep {
    WorldMapFeedbackOperation operation = WorldMapFeedbackOperation::None;
    uint64_t request_identity = 0;
    uint16_t feedback_id = 0;
    // Result snapshot before this operation; the argument for a Stop call.
    uint32_t playback_handle = UINT32_MAX;
    std::array<uint32_t, 3> arguments{};
    uint32_t return_word = 0;
};

// Native ownership of the bounded feedback request/list/result-word path.
// Identities are caller-supplied, nonzero, and stable while cursors are used.
// Destinations own supplied word snapshots, never original/host pointers.
// No game owner discovery, object pool, scheduling, or playback is supplied.
class WorldMapFeedbackRegistry {
public:
    [[nodiscard]] bool add_result_destination(uint64_t identity, uint32_t initial_word);
    [[nodiscard]] bool result_destination_word(uint64_t identity, uint32_t* out) const;
    // FUN_8018CCA0's field initialization; does not write the destination.
    [[nodiscard]] bool create_request(uint64_t identity, uint32_t feedback_id,
                                      std::optional<uint64_t> result_destination = std::nullopt);
    [[nodiscard]] bool request(uint64_t identity, WorldMapFeedbackRequest* out) const;
    // FUN_8018DD54: detach from any owner, then append to the selected list.
    [[nodiscard]] bool link_request(uint64_t identity, uint64_t owner_identity);
    [[nodiscard]] bool unlink_request(uint64_t identity);
    // FUN_8018DD08: unlink all, preserving requests/results/destinations.
    void clear_owner(uint64_t owner_identity);
    [[nodiscard]] size_t size(uint64_t owner_identity) const;
    // FUN_8018CD6C: low-halfword match, first in list or strictly after cursor.
    // A supplied cursor must belong to this owner. Failure preserves output.
    [[nodiscard]] bool find_request(uint64_t owner_identity, uint32_t feedback_id,
                                    std::optional<uint64_t> after, uint64_t* out) const;
    // FUN_8018DE94: reset result and its destination, unlink, then release.
    // The traced destructor does NOT call the playback-stop backend.
    [[nodiscard]] bool destroy_request(uint64_t identity);

    // FUN_8018B1F8 / FUN_8018B25C. No backend outcome means RequiresPlayback
    // with no state mutation. An outcome is an explicitly supplied observed
    // backend word, not generated success. Start writes both result locations
    // and returns result != -1; stop tests the low byte and returns the entire
    // backend word. Already-started start returns 0; unstarted stop returns 1.
    // Invalid input preserves output; these calls do not play or stop sound.
    [[nodiscard]] WorldMapFeedbackStatus start_request(uint64_t identity,
        const std::array<uint32_t, 3>& arguments, std::optional<uint32_t> backend_result,
        WorldMapFeedbackStep* out);
    [[nodiscard]] WorldMapFeedbackStatus stop_request(uint64_t identity,
        std::optional<uint32_t> backend_result, WorldMapFeedbackStep* out);

private:
    struct Destination { uint64_t identity; uint32_t word; };
    struct Link { uint64_t owner; uint64_t request; };
    std::vector<WorldMapFeedbackRequest> requests_;
    std::vector<Destination> destinations_;
    std::vector<Link> links_;
    void write_result(WorldMapFeedbackRequest& request, uint32_t word);
};

} // namespace awl
