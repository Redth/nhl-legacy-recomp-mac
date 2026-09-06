// See input_map.h.

#include "input_map.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

#include <rex/filesystem.h>
#include <rex/input/input.h>
#include <rex/logging.h>

namespace nhllegacy {

namespace {

struct ButtonDesc {
  GuestButton button;
  uint16_t mask;
  const char* name;   // display
  const char* key;    // ini key
};

// Order here is the UI order.
constexpr ButtonDesc kButtons[] = {
    {GuestButton::kA, rex::input::X_INPUT_GAMEPAD_A, "A", "a"},
    {GuestButton::kB, rex::input::X_INPUT_GAMEPAD_B, "B", "b"},
    {GuestButton::kX, rex::input::X_INPUT_GAMEPAD_X, "X", "x"},
    {GuestButton::kY, rex::input::X_INPUT_GAMEPAD_Y, "Y", "y"},
    {GuestButton::kLeftShoulder, rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER, "LB", "lb"},
    {GuestButton::kRightShoulder, rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER, "RB", "rb"},
    {GuestButton::kLeftThumb, rex::input::X_INPUT_GAMEPAD_LEFT_THUMB, "L3", "l3"},
    {GuestButton::kRightThumb, rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB, "R3", "r3"},
    {GuestButton::kStart, rex::input::X_INPUT_GAMEPAD_START, "Start", "start"},
    {GuestButton::kBack, rex::input::X_INPUT_GAMEPAD_BACK, "Back", "back"},
    {GuestButton::kDpadUp, rex::input::X_INPUT_GAMEPAD_DPAD_UP, "D-Pad Up", "dup"},
    {GuestButton::kDpadDown, rex::input::X_INPUT_GAMEPAD_DPAD_DOWN, "D-Pad Down", "ddown"},
    {GuestButton::kDpadLeft, rex::input::X_INPUT_GAMEPAD_DPAD_LEFT, "D-Pad Left", "dleft"},
    {GuestButton::kDpadRight, rex::input::X_INPUT_GAMEPAD_DPAD_RIGHT, "D-Pad Right", "dright"},
};
static_assert(sizeof(kButtons) / sizeof(kButtons[0]) == kGuestButtonCount,
              "kButtons must cover every GuestButton");

std::filesystem::path MapPath() {
  return rex::filesystem::GetExecutableFolder() / "nhl_input_map.ini";
}

}  // namespace

const char* GuestButtonName(GuestButton b) {
  const size_t i = static_cast<size_t>(b);
  return i < kGuestButtonCount ? kButtons[i].name : "?";
}

uint16_t GuestButtonMask(GuestButton b) {
  const size_t i = static_cast<size_t>(b);
  return i < kGuestButtonCount ? kButtons[i].mask : 0;
}

void InputMap::ResetToDefaults() {
  for (size_t i = 0; i < kGuestButtonCount; ++i) {
    source[i] = kButtons[i].mask;  // identity
  }
  swap_sticks = false;
  invert_left_y = false;
  invert_right_y = false;
  swap_triggers = false;
}

InputMap& CurrentInputMap() {
  static InputMap map;
  return map;
}

void ApplyInputMap(rex::input::X_INPUT_STATE& state) {
  const InputMap& m = CurrentInputMap();

  const uint16_t in = static_cast<uint16_t>(state.gamepad.buttons);
  uint16_t out = 0;
  // Preserve any bit we do not manage (e.g. GUIDE) so host features still work.
  uint16_t managed = 0;
  for (size_t i = 0; i < kGuestButtonCount; ++i) managed |= kButtons[i].mask;
  out |= static_cast<uint16_t>(in & ~managed);

  for (size_t i = 0; i < kGuestButtonCount; ++i) {
    // Guest button i fires when its configured SOURCE button is physically down.
    if (m.source[i] && (in & m.source[i])) {
      out |= kButtons[i].mask;
    }
  }
  state.gamepad.buttons = out;

  if (m.swap_triggers) {
    std::swap(state.gamepad.left_trigger, state.gamepad.right_trigger);
  }

  int16_t lx = state.gamepad.thumb_lx, ly = state.gamepad.thumb_ly;
  int16_t rx = state.gamepad.thumb_rx, ry = state.gamepad.thumb_ry;
  if (m.swap_sticks) {
    std::swap(lx, rx);
    std::swap(ly, ry);
  }
  // -32768 has no positive counterpart in int16; clamp so negation is lossless.
  auto invert = [](int16_t v) -> int16_t {
    return static_cast<int16_t>(-(v == INT16_MIN ? INT16_MIN + 1 : v));
  };
  if (m.invert_left_y) ly = invert(ly);
  if (m.invert_right_y) ry = invert(ry);
  state.gamepad.thumb_lx = lx;
  state.gamepad.thumb_ly = ly;
  state.gamepad.thumb_rx = rx;
  state.gamepad.thumb_ry = ry;
}

void LoadInputMap() {
  InputMap& m = CurrentInputMap();
  m.ResetToDefaults();

  std::ifstream f(MapPath());
  if (!f) return;  // no file yet: defaults are correct

  std::string line;
  while (std::getline(f, line)) {
    const size_t eq = line.find('=');
    if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
    const std::string key = line.substr(0, eq);
    const std::string val = line.substr(eq + 1);
    if (key == "swap_sticks") { m.swap_sticks = (val == "1"); continue; }
    if (key == "invert_left_y") { m.invert_left_y = (val == "1"); continue; }
    if (key == "invert_right_y") { m.invert_right_y = (val == "1"); continue; }
    if (key == "swap_triggers") { m.swap_triggers = (val == "1"); continue; }
    for (size_t i = 0; i < kGuestButtonCount; ++i) {
      if (key == kButtons[i].key) {
        m.source[i] = static_cast<uint16_t>(std::strtoul(val.c_str(), nullptr, 0));
        break;
      }
    }
  }
  REXLOG_INFO("[nhl-input] loaded controller map from {}", MapPath().string());
}

void SaveInputMap() {
  const InputMap& m = CurrentInputMap();
  std::ofstream f(MapPath(), std::ios::trunc);
  if (!f) {
    REXLOG_ERROR("[nhl-input] cannot write {}", MapPath().string());
    return;
  }
  f << "# NHL Legacy controller mapping.\n"
    << "# <guest button>=<physical button mask that drives it> (X_INPUT_GAMEPAD_* bit).\n";
  for (size_t i = 0; i < kGuestButtonCount; ++i) {
    f << kButtons[i].key << "=0x" << std::hex << m.source[i] << std::dec << "\n";
  }
  f << "swap_sticks=" << (m.swap_sticks ? 1 : 0) << "\n"
    << "invert_left_y=" << (m.invert_left_y ? 1 : 0) << "\n"
    << "invert_right_y=" << (m.invert_right_y ? 1 : 0) << "\n"
    << "swap_triggers=" << (m.swap_triggers ? 1 : 0) << "\n";
  REXLOG_INFO("[nhl-input] saved controller map to {}", MapPath().string());
}

}  // namespace nhllegacy
