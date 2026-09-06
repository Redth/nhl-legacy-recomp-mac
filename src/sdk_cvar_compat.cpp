// Game-side definitions for SDK cvars that a from-public-source runtime does
// not define. Same rationale and lifecycle as present_grade_compat.cpp — see
// that file's header comment.
//
// Two distinct reasons a cvar lands here:
//
//  1. Title-patch additions. shadow_filter_linear / shadow_softness (guest
//     depth-map filtering in the Vulkan texture cache) and trace_gpu_stream /
//     trace_gpu_prefix (PM4 stream dump) are defined in the original dev's SDK
//     tree. Neither appears in sdk/rexglue-vulkan-nhl-legacy-bd9b519.patch, so
//     they are missing from a stock build. They are consumed only SDK-side, so
//     until the patch is forward-ported these are inert: the overlay and
//     OnPreSetup can read/write them, but nothing acts on the value.
//
//  2. Backend-specific cvars compiled out on this platform.
//     render_target_path_d3d12 lives in the SDK's D3D12 render-target cache,
//     which is not built when REXGLUE_USE_D3D12=OFF (always, off-Windows). The
//     app sets it unconditionally in OnPreSetup; defining it here keeps that
//     path link-clean and the write is simply never read.
//
// Delete each block once the corresponding SDK-side definition exists again,
// or the duplicate definition will collide.

#include <rex/cvar.h>

// (1) Title-patch additions — inert until the patch is forward-ported.
REXCVAR_DEFINE_BOOL(shadow_filter_linear, false, "NHL",
                    "Bilinear filtering for guest depth/shadow maps (no-op "
                    "until the SDK-side texture-cache change is restored)");
REXCVAR_DEFINE_INT32(shadow_softness, 0, "NHL",
                     "Extra depth-domain blur passes for guest depth/shadow "
                     "maps, 0..4 (no-op until the SDK-side change is restored)");
REXCVAR_DEFINE_BOOL(trace_gpu_stream, false, "NHL",
                    "Dump the guest PM4 command stream (no-op until the "
                    "SDK-side change is restored)");
REXCVAR_DEFINE_STRING(trace_gpu_prefix, "", "NHL",
                      "Filename prefix for the PM4 stream dump");

// (2) D3D12-only cvar, absent whenever REXGLUE_USE_D3D12=OFF.
REXCVAR_DEFINE_STRING(render_target_path_d3d12, "rov", "NHL",
                      "D3D12 render-target path, \"rtv\" or \"rov\" (unused on "
                      "platforms without the D3D12 backend)");
