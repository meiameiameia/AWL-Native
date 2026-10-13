#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace awl {
using WorldMapPlayerContextVector=std::array<float,3>;
struct WorldMapPlayerContextSearchProfile {
    float angle_radians=0,first_min_distance=0,first_max_distance=0,other_min_distance=0,other_max_distance=0;
    std::array<float,4> region_xz{}; // 1CF3C's inclusive X min/max, Z min/max.
};
enum class WorldMapPlayerContextTablesStatus { Decoded, Loaded, InvalidInput, UnsupportedLayout, ReadFailure, WrongDol, AllocationFailure };
// Immutable 10A94's eight scan slots and 11634's sixteen keyed rows, plus
// 10F00's two facing thresholds. No runtime scene lists or raw DOL retained.
class WorldMapPlayerContextTables {
public:
    const std::array<uint32_t,8>& scan_slots() const noexcept { return scan_slots_; }
    bool target_verified() const noexcept { return target_verified_; }
    [[nodiscard]] bool neighbor(int32_t key,uint32_t slot,int32_t* out) const noexcept;
    float back_threshold() const noexcept { return back_threshold_; }
    float front_threshold() const noexcept { return front_threshold_; }
    // Optional in older query-only synthetic decodes; present in target DOL.
    const std::optional<WorldMapPlayerContextSearchProfile>& search_profile() const noexcept {return search_profile_;}
private:
    struct Row {int32_t key=0;std::array<int32_t,8> neighbors{};};
    WorldMapPlayerContextTables()=default;
    bool decode(const uint8_t*,size_t) noexcept;
    friend WorldMapPlayerContextTablesStatus decode_world_map_player_context_tables(
        const uint8_t*,size_t,std::shared_ptr<const WorldMapPlayerContextTables>*);
    friend WorldMapPlayerContextTablesStatus load_world_map_player_context_tables(
        std::shared_ptr<const WorldMapPlayerContextTables>*);
    std::array<uint32_t,8> scan_slots_{};
    std::array<Row,16> rows_{};
    float back_threshold_=0,front_threshold_=0;
    std::optional<WorldMapPlayerContextSearchProfile> search_profile_;
    bool target_verified_=false;
};
[[nodiscard]] WorldMapPlayerContextTablesStatus decode_world_map_player_context_tables(
    const uint8_t*,size_t,std::shared_ptr<const WorldMapPlayerContextTables>*);
[[nodiscard]] WorldMapPlayerContextTablesStatus load_world_map_player_context_tables(
    std::shared_ptr<const WorldMapPlayerContextTables>*);

// Observed live record fields in original list order. Missing fields are
// required only when reached. Payload is owner virtual +10's resolved two
// output words; its producer remains a separate scene-owner dependency.
struct WorldMapPlayerContextMetadata {
    std::optional<uint32_t> owner_code_4,owner_kind_8;
    std::optional<WorldMapPlayerContextVector> facing_28;
    std::optional<float> facing_threshold_38,max_squared_distance_3c;
    std::optional<std::array<uint32_t,2>> payload;
};
struct WorldMapPlayerContextCandidate : WorldMapPlayerContextMetadata {
    std::optional<WorldMapPlayerContextVector> position_1c;
};
struct WorldMapPlayerContextList {
    int32_t key=0;
    std::vector<WorldMapPlayerContextCandidate> candidates;
};
struct WorldMapPlayerContextQuery {
    int32_t current_key=0;
    std::optional<WorldMapPlayerContextVector> origin;
    WorldMapPlayerContextVector forward{};
    float angle_radians=0,min_distance=0,max_distance=0;
    uint32_t excluded_kinds=0;
};
struct WorldMapPlayerContextRecord {
    uint32_t owner_code=0,owner_kind=0;
    std::array<uint32_t,2> payload{UINT32_MAX,UINT32_MAX};
    WorldMapPlayerContextVector position{},facing{};
    uint32_t direction=0;
};
struct WorldMapPlayerContextResult {
    WorldMapPlayerContextRecord record;
    float nearest_squared_distance=0;
    bool found=false;
};
enum class WorldMapPlayerContextStatus {
    Prepared, RequiresTables, RequiresList, RequiresSceneRegistry, RequiresOrigin, RequiresOwnerKind,
    RequiresPosition, RequiresRange, RequiresFacingThreshold, RequiresFacing,
    RequiresOwnerCode, RequiresPayload, InvalidInput, UnsupportedNumerics
};
struct WorldMapPlayerContextStep {
    WorldMapPlayerContextStatus status=WorldMapPlayerContextStatus::InvalidInput;
    std::optional<int32_t> required_list;
    std::optional<size_t> required_candidate;
    uint32_t scanned_lists=0,visited_candidates=0;
};
// 10A94/10C9C/10F00 over supplied lists, using the owned DOL lookup tables.
// Current list precedes eight ordered neighbors; strict distance comparisons
// retain the earlier equal-distance candidate. Zero forward skips every list.
// Native paired-single FMA order and double reciprocal-sqrt seed + original
// rounded Newton step match the existing bounds profile. Native cos replaces
// original libc. Hardware estimate/libm corner equality remains uncalibrated;
// nearest rounding and normal/zero finite reached intermediates are required.
// No allocation or owner/global writes; every stop preserves *out. Missing
// lists are never treated as empty. No active state-0 callback is acknowledged.
[[nodiscard]] WorldMapPlayerContextStep query_world_map_player_context(
    const WorldMapPlayerContextTables*,const std::vector<WorldMapPlayerContextList>&,
    const WorldMapPlayerContextQuery&,WorldMapPlayerContextResult* out) noexcept;

class WorldMapSceneBucketRegistry;
// Same query over the registry's current intrusive-list equivalent. Positions
// and order come only from the registry; no snapshot/allocation or duplicate
// position storage. Missing reached metadata remains an explicit dependency.
// Null registry is required only when a nonzero/non-minus-one list is reached.
// Registry and observations must stay unchanged for this synchronous call.
[[nodiscard]] WorldMapPlayerContextStep query_world_map_player_scene_context(
    const WorldMapPlayerContextTables*,const WorldMapSceneBucketRegistry*,
    const WorldMapPlayerContextQuery&,WorldMapPlayerContextResult* out) noexcept;
} // namespace awl
