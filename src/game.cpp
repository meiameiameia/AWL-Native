#include "awl/game.h"
#include "awl/platform.h"

namespace awl {

void game_init() {
    AWL_LOG_INFO("Game layer initialized; original gameplay systems remain untranslated.");
}

void game_update(double delta_time) {
    (void)delta_time;
    // Native input is captured each frame, but no original player update has
    // been translated yet. Do not infer game actions from the device state.
}

void game_shutdown() {
    AWL_LOG_INFO("Game layer shut down.");
}

} // namespace awl
