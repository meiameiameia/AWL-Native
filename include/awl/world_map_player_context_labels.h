#pragma once
#include "awl/world_map_player_context.h"

namespace awl {
enum class WorldMapPlayerContextLabelTablesStatus { Decoded, Loaded, InvalidInput, UnsupportedLayout, ReadFailure, WrongDol, AllocationFailure };
// 4ED60's 31 rows of two 86-byte label arrays, plus 4EFDC's final
// item-type routing and the reached forward-probe distance. No raw DOL kept.
class WorldMapPlayerContextLabelTables {
public:
    bool target_verified() const noexcept {return target_verified_;}
    [[nodiscard]] bool labels(uint32_t classification,uint32_t context,std::array<uint32_t,2>* out) const noexcept;
    uint32_t item_class(uint8_t type) const noexcept {return type<item_classes_.size()?item_classes_[type]:0;}
    float probe_distance() const noexcept {return probe_distance_;}
private:
    WorldMapPlayerContextLabelTables()=default;
    bool decode(const uint8_t*,size_t) noexcept;
    friend WorldMapPlayerContextLabelTablesStatus decode_world_map_player_context_label_tables(
        const uint8_t*,size_t,std::shared_ptr<const WorldMapPlayerContextLabelTables>*);
    friend WorldMapPlayerContextLabelTablesStatus load_world_map_player_context_label_tables(
        std::shared_ptr<const WorldMapPlayerContextLabelTables>*);
    std::array<std::array<uint8_t,172>,31> rows_{};
    std::array<uint8_t,31> item_classes_{};
    float probe_distance_=0;
    bool target_verified_=false;
};
[[nodiscard]] WorldMapPlayerContextLabelTablesStatus decode_world_map_player_context_label_tables(
    const uint8_t*,size_t,std::shared_ptr<const WorldMapPlayerContextLabelTables>*);
[[nodiscard]] WorldMapPlayerContextLabelTablesStatus load_world_map_player_context_label_tables(
    std::shared_ptr<const WorldMapPlayerContextLabelTables>*);

enum class WorldMapPlayerContextLabelSource { HeldItem, Action };
struct WorldMapPlayerContextItemRow {
    int32_t item_id=0;
    std::optional<uint8_t> type_0,flags_1;
    std::optional<uint32_t> flags_1c;
};
struct WorldMapPlayerContextLabelInput {
    WorldMapPlayerContextLabelSource source=WorldMapPlayerContextLabelSource::HeldItem;
    int32_t scene_type=0,value=0,target_kind=-1,target_payload=-1;
    // target_kind/target_payload are the two resolved virtual payload words
    // (descriptor +1C/+20), distinct from the node's owner_kind_8.
    WorldMapPlayerContextVector position{},facing{};
    uint32_t direction=0;
    // Constructor r7 == 1 uses fixed context 42; all other values reach
    // 4F2EC's target classification (including the active callback's zero).
    int32_t query_mode=0;
    std::optional<WorldMapPlayerContextItemRow> item_row;
    // 8E390/8E418's type-8/9 resolution precedes even early item returns.
    // Its saved-record producer remains supplied, not inferred from item IDs.
    std::optional<bool> item_record_resolved;
    std::optional<uint8_t> item_record_byte_c;
    // 23988 is reached for every action-source target kind 30, even when
    // the following 4F25C branch does not read flags +28.
    std::optional<bool> action_record_resolved;
    std::optional<uint8_t> action_record_flags_28;
    std::optional<bool> target_record_resolved,target_record_predicate;
    std::optional<uint8_t> target_record_flags_28;
    // 4ED60's ordered context-74/75, context-8 and context-47 checks.
    std::array<std::optional<bool>,2> context_item_available;
    std::optional<bool> context8_scene_blocked,item_flag1_available;
    // Resolved 4F8CC results for the ordered primary/secondary label-29
    // probes at the position reported in Step. These observations do not
    // execute the collision body.
    std::array<std::optional<bool>,2> terrain_clear;
    std::optional<bool> secondary_ground_available; // reached 1CCB8 for label 25
};
struct WorldMapPlayerContextLabels {
    int32_t scene_type=0;
    WorldMapPlayerContextVector position{},facing{};
    int32_t target_kind=-1,target_payload=-1;
    uint32_t direction=0;
    int32_t item=0,action=0;
    std::array<uint32_t,3> labels{}; // descriptor +30/+34/+38
};
enum class WorldMapPlayerContextLabelStatus {
    Prepared, RequiresTables, RequiresItemRow, RequiresItemType, RequiresItemRecord,
    RequiresItemFlags, RequiresItemRecordByte, RequiresTerrain, RequiresGround,
    RequiresActionRecord, RequiresActionFlags,
    RequiresTargetRecord, RequiresTargetPredicate, RequiresTargetFlags,
    RequiresItemAvailability, RequiresScenePredicate,
    InvalidInput, UnsupportedNumerics
};
struct WorldMapPlayerContextLabelStep {
    WorldMapPlayerContextLabelStatus status=WorldMapPlayerContextLabelStatus::InvalidInput;
    uint32_t classification=0,context=42;
    uint8_t probe_mask=0; // primary/secondary forward probes, then ground probe
    std::optional<uint32_t> terrain_channel;
    std::optional<WorldMapPlayerContextVector> terrain_position;
};
// 45594/4567C constructors through 4EFDC/4F210, target 4F2EC or fixed
// 4F8C4, reached 4ED60 label logic and 4F25C adjustment. Explicit
// reached saved-record/terrain observations; no action dispatch or UI writes.
// Preserves *out on every stop, uses no allocation, and never acknowledges
// the surrounding active state-zero third callback.
[[nodiscard]] WorldMapPlayerContextLabelStep prepare_world_map_player_context_labels(
    const WorldMapPlayerContextLabelTables*,const WorldMapPlayerContextLabelInput&,
    WorldMapPlayerContextLabels* out) noexcept;
// Binds the query's two payload words and direction, including its no-hit
// defaults; retains the caller's justified actor pose and other observations.
[[nodiscard]] WorldMapPlayerContextLabelStep prepare_world_map_player_context_labels_from_target(
    const WorldMapPlayerContextLabelTables*,const WorldMapPlayerContextResult&,
    const WorldMapPlayerContextLabelInput&,WorldMapPlayerContextLabels* out) noexcept;
} // namespace awl
