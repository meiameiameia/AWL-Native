#pragma once
#include "awl/world_map_player_model_assets.h"
#include "awl/world_map_player_model_setup.h"
#include "awl/world_map_player_timer_assets.h"

namespace awl {
enum class WorldMapTextureAnimationSampleStatus { Sampled, InvalidInput, UnsupportedLayout, UnsupportedNumerics };
struct WorldMapTextureAnimationSample {
    uint32_t current = 0, next = 0, texture_index = 0;
};
enum class WorldMapTextureTimerStatus {
    Prepared, RequiresFeature, RequiresClock, RequiresTimerAssets, RequiresResource,
    RequiresMouthGate, UnsupportedLayout, UnsupportedNumerics, InvalidInput
};
struct WorldMapTextureTimerResult {
    WorldMapTextureTimerStatus status = WorldMapTextureTimerStatus::InvalidInput;
    std::optional<uint32_t> failed_timer; // 0 eyes, 1 mouth; absent before/after reached work.
    uint8_t updated_mask = 0;
};
// E03C: rounded unsigned (now-clock)*rate/1000. At value >= resource +4's
// duration, subtract duration ONCE and reset clock to now. Old value is unread;
// zero/negative duration/rate are retained finite arithmetic, not repaired.
// Only the resource header is needed here; record sampling is separate.
// Default nearest rounding and finite normal/zero reached values required.
// Failure preserves out; whole timer input/output aliasing is supported.
[[nodiscard]] WorldMapTextureTimerStatus prepare_world_map_texture_timer(
    const WorldMapAnimationFeatureTimer& timer, const WorldMapPlayerTimerResourceView& resource,
    uint32_t now, WorldMapAnimationFeatureTimer* out) noexcept;
// F6D4's signed type/index gates, first then second. Mouth type >=24 reads
// byte +34 before falling back to index >=0; missing reached byte must stop.
// Resolve exact keys from common eye/mouth banks. Unreached timers/clock/assets
// and numerical mode are ignored. Late failure rolls back both timers.
[[nodiscard]] WorldMapTextureTimerResult prepare_world_map_player_texture_timers(
    const WorldMapAnimationFeature38& feature, std::optional<uint8_t> mouth_gate,
    const WorldMapPlayerTimerAssets* assets, std::optional<uint32_t> now,
    WorldMapAnimationFeature38* out) noexcept;
// 82F8/E0C4: count at +0, then (texture word, time float) pairs at +8.
// Select the first segment whose NEXT time is strictly greater than value;
// otherwise select the last record. A single record reads no time/value.
// Header +4, current time and later unused records remain opaque. No clock
// advance, interpolation or implicit looping. Failure preserves output.
[[nodiscard]] WorldMapTextureAnimationSampleStatus sample_world_map_texture_animation(
    const WorldMapPlayerTimerResourceView& resource, float value, WorldMapTextureAnimationSample* out) noexcept;

struct WorldMapPlayerTextureFeatureInput {
    std::optional<WorldMapAnimationFeature38> feature;
    std::optional<uint32_t> source_10;
    const WorldMapPlayerTimerAssets* timers = nullptr; // Borrowed for this call only.
};
enum class WorldMapPlayerTextureFeatureStatus {
    Selected, RequiresFeature, RequiresTimerAssets, RequiresResource, RequiresSource,
    InvalidTexture, InvalidInput, UnsupportedLayout, UnsupportedNumerics
};
struct WorldMapPlayerTextureFeatureResult {
    WorldMapPlayerTextureFeatureStatus status = WorldMapPlayerTextureFeatureStatus::InvalidInput;
    std::optional<uint32_t> failed_channel;
    std::array<std::optional<WorldMapPlayerModelTextureBinding>,3> textures;
    uint32_t cache_writes = 0; // Filled by the CPU primary, including repeated stores.
};
struct WorldMapPlayerTextureCacheEntry {
    uint32_t channel = UINT32_MAX;
    std::optional<WorldMapPlayerModelTextureBinding> texture;
};
// F548 selection in original order: resolve first/second indices, select eyes
// then mouth, then source 0x16's body route. Signed index >=0 samples both timers;
// otherwise signed types >=0x3A/0x24 sample, and smaller types select directly.
// Only retained common eye/mouth resource identities resolve here. Other
// resources (including private TAM timers) stop explicitly. No timer writes,
// cache publication, actor effects, holder progression or queue completion.
[[nodiscard]] WorldMapPlayerTextureFeatureResult prepare_world_map_player_texture_features(
    const WorldMapPlayerTextureFeatureInput& input, const WorldMapPlayerModelAssets& assets) noexcept;
// DCD8/2B74/36DC over the retained root then ordered node features, for each
// selected channel. Matching type-one commands store into the first existing
// channel, else first free slot; full cache is a successful no-op. Repeated
// commands retain repeated stores. Whole-cache input/output alias is supported.
// Neither the construction commands nor their texture bindings are changed.
[[nodiscard]] bool prepare_world_map_player_texture_cache(const WorldMapPlayerModelSetupPlan& plan,
    const WorldMapPlayerTextureFeatureResult& selected,
    const std::optional<std::array<WorldMapPlayerTextureCacheEntry,4>>& before,
    std::optional<std::array<WorldMapPlayerTextureCacheEntry,4>>* out, uint32_t* writes) noexcept;
} // namespace awl
