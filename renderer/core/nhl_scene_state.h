// Scene classification and edge-AA policy, shared between the Vulkan backend
// (which decides) and the enhancements overlay (which displays and overrides).
//
// Deliberately dependency-light — like nhl_tunable_store.h and nhl_settings.h,
// it must be includable from the overlay TU without dragging in the SDK's
// Vulkan headers. The implementation lives in nhl_vk_backend.cpp.

#pragma once

#include <cstdint>

namespace nhl::graphics {

// What the guest is currently drawing, decided per frame in IssueSwap.
//
// The discriminator is the guest's own render-surface geometry, not a heuristic
// draw-count threshold (draw counts overlap badly between busy menus and light
// gameplay). NHL renders the 3D scene through predicated tiling: two 640-pitch
// EDRAM passes composed into one 1280-wide destination. Menus and 2D overlays
// render at the full 1280 pitch. So "this frame issued draws against a
// 640-pitch surface" is an exact statement about the 3D scene being present,
// and it is the same test the tile-depth-clear and un-fold-skip fixes key off.
enum class NhlSceneKind { kMenu, kScene3D };
void PublishSceneKind(NhlSceneKind kind);
NhlSceneKind ReadSceneKind();

// How the edge-AA/bloom pixel shader (REX_SKIP_PS) is gated. It looks right
// over the flat 2D menus but blows highlights out to white over the 3D scene,
// so the default follows the scene detector above. Order matches the persisted
// `edge_aa_mode` ini value and the overlay's combo.
enum class NhlEdgeAaMode { kAuto = 0, kAlways = 1, kNever = 2 };
void SetEdgeAaMode(NhlEdgeAaMode mode);
NhlEdgeAaMode GetEdgeAaMode();

// The SDK's swap-time post effect, mirrored here so the overlay can drive it
// without including the SDK's Vulkan headers. The SDK reads its swap_post_effect
// cvar once at context setup, so changing that string later does nothing; the
// backend applies this value via SetDesiredSwapPostEffect instead, which is why
// this is genuinely live where Supersampling is not.
//
// FXAA is edge-directed and does smooth geometry edges, but note it does NOT
// remove the sub-pixel checker on boards/glass/ice - that needs supersampling.
// It also softens menu and HUD text, since it runs on the whole frame.
enum class NhlSwapPostEffect { kNone = 0, kFxaa = 1, kFxaaExtreme = 2 };
void SetSwapPostEffect(NhlSwapPostEffect effect);
NhlSwapPostEffect GetSwapPostEffect();

// Destination of the most recent full-size (>=1280 pitch) colour resolve, so a
// dump can target whatever buffer the game is actually using right now. Fixed
// addresses read out of an earlier run's log go stale the moment the game moves
// its buffers - three dumps came back all-zero that way.
struct NhlResolveTarget {
  uint32_t address = 0;
  uint32_t pitch = 0;
  uint32_t height = 0;
};
void PublishLastColorResolve(const NhlResolveTarget& t);
NhlResolveTarget ReadLastColorResolve();

}  // namespace nhl::graphics
