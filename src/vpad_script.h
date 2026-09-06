// Scripted virtual gamepad (NHL_VPAD_SCRIPT), for headless/automated testing.
//
// Attaches an SDL3 *virtual* gamepad to this process and presses buttons on a
// timeline. SDL3's virtual joystick driver is enabled in the SDK's SDL build,
// and the SDK's SDL input driver already handles SDL_EVENT_GAMEPAD_ADDED, so
// the synthetic pad travels the real path: SDL -> rex::input::SDLInputDriver ->
// XamInputGetState -> guest. That makes it a genuine end-to-end input test, not
// a bypass.
//
// Format: comma-separated <seconds>:<button>[:<hold_ms>] entries, e.g.
//   NHL_VPAD_SCRIPT="6:start,9:a,12:a:250"
// Button names: a b x y start back guide up down left right lb rb ls rs.

#pragma once

namespace nhllegacy {

// No-op unless NHL_VPAD_SCRIPT is set. Spawns a detached driver thread.
void StartVpadScript();

}  // namespace nhllegacy
