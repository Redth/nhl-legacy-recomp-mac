// Scene classification and edge-AA policy, shared between the Vulkan backend
// (which decides) and the enhancements overlay (which displays and overrides).
//
// Deliberately dependency-light — like nhl_tunable_store.h and nhl_settings.h,
// it must be includable from the overlay TU without dragging in the SDK's
// Vulkan headers. The implementation lives in nhl_vk_backend.cpp.

#pragma once

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

}  // namespace nhl::graphics
