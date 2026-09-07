// See vpad_script.h.

#include "vpad_script.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>

#include "renderer/core/nhl_vk_backend.h"

namespace nhllegacy {

namespace {

struct Press {
  unsigned at_ms = 0;
  SDL_GamepadButton button = SDL_GAMEPAD_BUTTON_SOUTH;
  unsigned hold_ms = 150;
};

SDL_GamepadButton ButtonByName(const std::string& n) {
  // Xbox naming on the left, SDL3's position-neutral names on the right.
  if (n == "a") return SDL_GAMEPAD_BUTTON_SOUTH;
  if (n == "b") return SDL_GAMEPAD_BUTTON_EAST;
  if (n == "x") return SDL_GAMEPAD_BUTTON_WEST;
  if (n == "y") return SDL_GAMEPAD_BUTTON_NORTH;
  if (n == "back") return SDL_GAMEPAD_BUTTON_BACK;
  if (n == "guide") return SDL_GAMEPAD_BUTTON_GUIDE;
  if (n == "start") return SDL_GAMEPAD_BUTTON_START;
  if (n == "ls") return SDL_GAMEPAD_BUTTON_LEFT_STICK;
  if (n == "rs") return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
  if (n == "lb") return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
  if (n == "rb") return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
  if (n == "up") return SDL_GAMEPAD_BUTTON_DPAD_UP;
  if (n == "down") return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
  if (n == "left") return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
  if (n == "right") return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
  return SDL_GAMEPAD_BUTTON_INVALID;
}

std::vector<Press> ParseScript(const char* spec, bool frame_based) {
  std::vector<Press> out;
  std::string s(spec);
  size_t pos = 0;
  while (pos <= s.size()) {
    const size_t comma = s.find(',', pos);
    std::string item = s.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
    if (!item.empty()) {
      const size_t c1 = item.find(':');
      if (c1 != std::string::npos) {
        Press p;
        // In frame mode at_ms holds a frame index, not milliseconds.
        p.at_ms = frame_based
                      ? static_cast<unsigned>(std::strtoul(item.substr(0, c1).c_str(), nullptr, 10))
                      : static_cast<unsigned>(
                            std::strtod(item.substr(0, c1).c_str(), nullptr) * 1000.0);
        const size_t c2 = item.find(':', c1 + 1);
        const std::string name = item.substr(c1 + 1, c2 == std::string::npos ? std::string::npos
                                                                             : c2 - c1 - 1);
        if (c2 != std::string::npos) {
          p.hold_ms = static_cast<unsigned>(std::strtoul(item.substr(c2 + 1).c_str(), nullptr, 10));
        }
        p.button = ButtonByName(name);
        if (p.button != SDL_GAMEPAD_BUTTON_INVALID) out.push_back(p);
        else REXLOG_WARN("[nhl-vpad] unknown button '{}'", name);
      }
    }
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
  std::sort(out.begin(), out.end(), [](const Press& a, const Press& b) { return a.at_ms < b.at_ms; });
  return out;
}

}  // namespace

void StartVpadScript() {
  // NHL_VPAD_SCRIPT_FRAMES uses GUEST FRAME indices instead of seconds. The
  // wall-clock form makes runs diverge: if two runs render at different speeds
  // the presses land on different guest frames, so the simulation goes a
  // different way and A/B captures are not comparable. Driving input off the
  // frame counter makes the whole run reproducible.
  const bool frame_based = std::getenv("NHL_VPAD_SCRIPT_FRAMES") != nullptr;
  const char* spec =
      frame_based ? std::getenv("NHL_VPAD_SCRIPT_FRAMES") : std::getenv("NHL_VPAD_SCRIPT");
  if (!spec || !*spec) return;
  std::vector<Press> script = ParseScript(spec, frame_based);
  if (script.empty()) {
    REXLOG_WARN("[nhl-vpad] NHL_VPAD_SCRIPT parsed to nothing: '{}'", spec);
    return;
  }

  std::thread([script = std::move(script), frame_based]() {
    // The SDK's SDL input driver owns SDL_INIT_GAMEPAD; wait for it rather than
    // racing it, otherwise the attach lands before the driver can observe it.
    for (int i = 0; i < 400 && !SDL_WasInit(SDL_INIT_GAMEPAD); ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!SDL_WasInit(SDL_INIT_GAMEPAD)) {
      REXLOG_ERROR("[nhl-vpad] SDL gamepad subsystem never came up; no virtual pad");
      return;
    }

    SDL_VirtualJoystickDesc desc;
    SDL_INIT_INTERFACE(&desc);
    desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    desc.naxes = 6;
    desc.nbuttons = 15;
    desc.nhats = 1;
    desc.vendor_id = 0x045e;   // Microsoft
    desc.product_id = 0x028e;  // Xbox 360 pad, so SDL maps it to the standard layout
    desc.name = "NHL Virtual Pad";

    const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
    if (!id) {
      REXLOG_ERROR("[nhl-vpad] SDL_AttachVirtualJoystick failed: {}", SDL_GetError());
      return;
    }
    SDL_Joystick* js = SDL_OpenJoystick(id);
    if (!js) {
      REXLOG_ERROR("[nhl-vpad] SDL_OpenJoystick failed: {}", SDL_GetError());
      SDL_DetachVirtualJoystick(id);
      return;
    }
    REXLOG_INFO("[nhl-vpad] virtual pad attached (id={}, gamepad={}), {} scripted press(es)",
                static_cast<unsigned>(id), SDL_IsGamepad(id) ? "yes" : "no", script.size());
    // Give the driver a moment to process SDL_EVENT_GAMEPAD_ADDED and slot it.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    unsigned now_ms = 0;
    for (const Press& p : script) {
      if (frame_based) {
        while (nhl::graphics::ReadVkFrameIndex() < p.at_ms) {
          std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
      } else if (p.at_ms > now_ms) {
        std::this_thread::sleep_for(std::chrono::milliseconds(p.at_ms - now_ms));
        now_ms = p.at_ms;
      }
      SDL_SetJoystickVirtualButton(js, p.button, true);
      std::this_thread::sleep_for(std::chrono::milliseconds(p.hold_ms));
      SDL_SetJoystickVirtualButton(js, p.button, false);
      now_ms += p.hold_ms;
      REXLOG_INFO("[nhl-vpad] at={} pressed {} for {}ms (guest frame {})", p.at_ms,
                  SDL_GetGamepadStringForButton(p.button), p.hold_ms,
                  nhl::graphics::ReadVkFrameIndex());
    }
    REXLOG_INFO("[nhl-vpad] script complete; pad stays attached");
  }).detach();
}

}  // namespace nhllegacy
