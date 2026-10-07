#pragma once
#include "awl/world_map_player_start.h"
#include "awl/world_map_player_animation_assets.h"

namespace awl {
struct WorldMapPlayerStartItemType { uint32_t item = 0; uint8_t type = 0; };
// FUN_80029138 after its metadata-byte read: types 1/2/4 or item 4FF
// choose 3; another nonzero item chooses 1; action alone chooses 2; else 0.
[[nodiscard]] uint32_t classify_world_map_player_start_animation(
    uint32_t item, uint32_t action, uint8_t item_type);

struct WorldMapPlayerStartAnimationSelection {
    uint32_t choice = 0;
    uint16_t descriptor_index = 0;
    WorldMapActorAnimationDescriptor descriptor;
};
enum class WorldMapPlayerStartAnimationStatus {
    Decoded, Loaded, Selected, Prepared, Unchanged, InvalidInput,
    UnsupportedLayout, UnsupportedCommand, RequiresTables, RequiresItemType,
    RequiresAssets, RequiresGroup, InitializerIncomplete,
    ReadFailure, WrongDol, AllocationFailure, Advanced
};

// Immutable first records of selector 1's four descriptor choices. Logical
// DOL addresses are equality keys, never host pointers. These copied words
// do not decode the subsequent 12-byte sequence or establish playback.
class WorldMapPlayerStartAnimationTables {
public:
    WorldMapPlayerStartAnimationTables(const WorldMapPlayerStartAnimationTables&) = delete;
    WorldMapPlayerStartAnimationTables& operator=(const WorldMapPlayerStartAnimationTables&) = delete;
    bool target_verified() const { return target_verified_; }
    // Selector 1 ignores variant/mode. Item zero's type comes from the
    // owned table snapshot; other items require keyed observed metadata.
    // Stops preserve output; no item table is inferred from mapped extent.
    [[nodiscard]] WorldMapPlayerStartAnimationStatus select(
        const WorldMapPlayerStartAnimationCommand& command,
        const std::optional<WorldMapPlayerStartItemType>& item_type,
        WorldMapPlayerStartAnimationSelection* out) const;
private:
    WorldMapPlayerStartAnimationTables() = default;
    bool decode(const uint8_t* data, size_t size);
    friend WorldMapPlayerStartAnimationStatus decode_world_map_player_start_animation_tables(
        const uint8_t*, size_t, std::shared_ptr<const WorldMapPlayerStartAnimationTables>*);
    friend WorldMapPlayerStartAnimationStatus load_world_map_player_start_animation_tables(
        std::shared_ptr<const WorldMapPlayerStartAnimationTables>*);
    std::array<WorldMapPlayerStartAnimationSelection,4> entries_{};
    uint8_t empty_item_type_ = 0;
    bool target_verified_ = false;
};
// Bounded DOL section/table decoder for supplied bytes, including synthetic
// fixtures. Validates extents, overlaps, jump target, indices and pointers;
// it does NOT verify target identity. Failures preserve existing ownership.
[[nodiscard]] WorldMapPlayerStartAnimationStatus decode_world_map_player_start_animation_tables(
    const uint8_t* data, size_t size, std::shared_ptr<const WorldMapPlayerStartAnimationTables>* out);
// Reads mounted /sys/main.dol, verifies SHA1 of those exact bytes, then
// decodes. Only a previously target-verified owner skips I/O. No raw DOL
// bytes are retained or published; all failures preserve the old owner.
[[nodiscard]] WorldMapPlayerStartAnimationStatus load_world_map_player_start_animation_tables(
    std::shared_ptr<const WorldMapPlayerStartAnimationTables>* out);

struct WorldMapPlayerStartAnimationStep {
    std::shared_ptr<const WorldMapPlayerStartAnimationTables> tables;
    std::optional<WorldMapPlayerStartAnimationSelection> selection;
    std::optional<WorldMapPlayerAnimationGroupBinding> binding;
    std::optional<WorldMapAnimationInitializerStatus> initializer_status;
    std::optional<WorldMapAnimationInitializerStep> initializer;
};
// FUN_8007C0C0 -> selector 1 -> FUN_8007C870/8017D660, composing the
// existing supplied-state initializer. Equal base skips asset/group/setup
// reads AFTER selection. Changed base retains group-zero banks and reports
// the exact reached initializer dependency. Prepared is metadata only:
// no native model/channel mutation, counter reset or parent-state acceptance.
// Invalid/allocating failures preserve output; incomplete initializer steps
// are diagnostic prefixes, never resumable acknowledgements. Reprepare from
// the original state. The input and output initializer state must not alias.
[[nodiscard]] WorldMapPlayerStartAnimationStatus prepare_world_map_player_start_animation(
    const std::shared_ptr<const WorldMapPlayerStartAnimationTables>& tables,
    const WorldMapPlayerStartAnimationCommand& command,
    const std::optional<WorldMapPlayerStartItemType>& item_type,
    const WorldMapAnimationInitializerState& state,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& assets,
    const std::optional<WorldMapAnimationModelBinding>& model,
    const WorldMapAnimationInitializerObservations& observations,
    WorldMapPlayerStartAnimationStep* out);

struct WorldMapPlayerStartNativeAnimationStep {
    std::shared_ptr<const WorldMapPlayerStartAnimationTables> tables;
    std::optional<WorldMapPlayerStartAnimationSelection> selection;
    std::optional<WorldMapPlayerAnimationGroupBinding> binding;
    WorldMapNativeAnimationInitializerStep initializer;
};
// Select first, then atomically initialize supplied native no-skin model/channel
// owners with their authoritative partial records/link graph. Advanced applies
// only that bounded transaction, not real primary construction, parent state 29,
// counter reset, frame evaluation or original gameplay acceptance. Reached
// secondary paths still block. Invalid/allocation failure preserves output.
[[nodiscard]] WorldMapPlayerStartAnimationStatus advance_world_map_player_start_animation(
    const std::shared_ptr<const WorldMapPlayerStartAnimationTables>& tables,
    const WorldMapPlayerStartAnimationCommand& command,
    const std::optional<WorldMapPlayerStartItemType>& item_type,
    WorldMapNativeAnimationInitializerMetadata* metadata, WorldMapNativeAnimationChannel* channel,
    const std::vector<WorldMapNativeModel*>& models,
    const std::shared_ptr<const WorldMapPlayerAnimationAssets>& assets,
    const WorldMapAnimationInitializerObservations& observations,
    WorldMapPlayerStartNativeAnimationStep* out);
} // namespace awl
