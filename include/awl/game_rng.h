#pragma once
#include "awl/game_clock.h"
#include <memory>
#include <optional>
#include <utility>

namespace awl {
// Complete 8017D3E8, shared with 8017D418; unsigned word arithmetic.
[[nodiscard]] uint32_t advance_game_rng(uint32_t* seed) noexcept;
// One process-wide owner of r13-5508, initially zero from CRT .sbss clearing.
// Keep it across scenes. All translated consumers must use the same owner.
class GameRng {
public:
    explicit GameRng(std::shared_ptr<GameClock> clock) noexcept : clock_(std::move(clock)) {}
    const std::shared_ptr<GameClock>& game_clock() const noexcept {return clock_;}
    uint32_t seed() const noexcept {return seed_;}
    [[nodiscard]] uint32_t draw() noexcept {return advance_game_rng(&seed_);}
    // 8000AC54 discards one draw on every outer game loop, before callbacks.
    // Receipt makes a native retry idempotent. It is not a render-frame hook.
    [[nodiscard]] bool begin_loop() noexcept;
    bool loop_started() const noexcept;
private:
    friend class WorldMapSceneAssembly;
    friend class WorldMapSceneConstructorTables;
    std::shared_ptr<GameClock> clock_;
    uint32_t seed_=0;
    std::optional<uint32_t> loop_;
};
} // namespace awl
