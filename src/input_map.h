// User-configurable controller remapping for the guest (Xbox 360) pad.
//
// The guest only ever sees an Xbox 360 controller, so "remapping" means:
// which PHYSICAL button should produce each GUEST button. The mapping is
// applied game-side in the XamInputGetState override (src/input_block.cpp),
// so it needs no SDK changes and cannot affect the host UI's own navigation.
//
// Sticks/triggers get swap + invert toggles rather than free remapping, which
// is what actually comes up in practice (southpaw, inverted camera, swapped
// triggers) and keeps the UI small.
//
// Persisted to nhl_input_map.ini next to the executable.

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace rex::input {
struct X_INPUT_STATE;
}

namespace nhllegacy {

// The guest buttons a user can rebind, in display order. GUIDE is deliberately
// excluded: the host overlay owns it.
enum class GuestButton : uint8_t {
  kA, kB, kX, kY,
  kLeftShoulder, kRightShoulder,
  kLeftThumb, kRightThumb,
  kStart, kBack,
  kDpadUp, kDpadDown, kDpadLeft, kDpadRight,
  kCount,
};

inline constexpr size_t kGuestButtonCount = static_cast<size_t>(GuestButton::kCount);

const char* GuestButtonName(GuestButton b);
uint16_t GuestButtonMask(GuestButton b);

struct InputMap {
  // source[i] = the guest-button mask that should DRIVE guest button i.
  // Identity by default (A->A, B->B, ...). Stored as masks so a physical
  // button can drive several guest buttons if the user wants.
  std::array<uint16_t, kGuestButtonCount> source{};

  bool swap_sticks = false;
  bool invert_left_y = false;
  bool invert_right_y = false;
  bool swap_triggers = false;

  InputMap() { ResetToDefaults(); }
  void ResetToDefaults();
};

// Process-wide mapping used by the XamInputGetState override.
InputMap& CurrentInputMap();

// Rewrites `state` in place per CurrentInputMap(). Cheap; safe to call per poll.
void ApplyInputMap(rex::input::X_INPUT_STATE& state);

// Persistence next to the executable. Both are no-fail (log on error).
void LoadInputMap();
void SaveInputMap();

}  // namespace nhllegacy
