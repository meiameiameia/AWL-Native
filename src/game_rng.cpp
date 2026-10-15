#include "awl/game_rng.h"

namespace awl {
uint32_t advance_game_rng(uint32_t* seed) noexcept {
    if(!seed)return 0;
    const uint32_t first=*seed*0x41c64e6du+0x3039u;
    *seed=first*0x41c64e6du+0x3039u;
    return (*seed&0xffff0000u)|(first>>16);
}
bool GameRng::loop_started() const noexcept {
    return clock_ && loop_ && *loop_==clock_->state().loop_count;
}
bool GameRng::begin_loop() noexcept {
    if(!clock_)return false;
    if(!loop_started()){(void)draw();loop_=clock_->state().loop_count;}
    return true;
}
} // namespace awl
