// nhllegacy - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <unistd.h>  // _exit, for nhl::compat::HardExit
#endif

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/ui/flags.h>
#include <rex/ui/presenter.h>
#include <rex/graphics/pipeline/texture/util.h>

// The D3D12 graphics system is Windows-only: off-Windows the SDK builds with
// REXGLUE_USE_D3D12=OFF and rex/graphics/d3d12/* pulls in windows.h. Vulkan is
// the only backend there, so NHL_HAVE_VULKAN_BACKEND is always defined and the
// D3D12 fallback below is unreachable.
#ifdef _WIN32
#define NHL_HAVE_D3D12_BACKEND 1
#include "renderer/core/nhl_graphics_system.h"
#endif

namespace nhl::compat {
// _putenv_s is MSVC-only; setenv is the POSIX spelling. Same semantics here
// (overwrite an existing value), so the call sites stay one-liners.
inline void SetEnv(const char* name, const char* value) {
#ifdef _WIN32
  _putenv_s(name, value);
#else
  ::setenv(name, value, /*overwrite=*/1);
#endif
}
}  // namespace nhl::compat
// SPIKE (docs/vulkan-rov-backend-spike-prompt.md): the SDK's Vulkan ROV backend
// is a COMPILE-TIME option (REXGLUE_USE_VULKAN, OFF in the stock win-amd64 zips).
// The header always installs; the implementation only links against an SDK built
// with the flag ON. NHL_HAVE_VULKAN_BACKEND is defined by the build that links
// such an SDK, gating both the include and the runtime NHL_VK_BACKEND env gate so
// the default (D3D12-only) build stays link-clean.
#ifdef NHL_HAVE_VULKAN_BACKEND
#include <rex/graphics/vulkan/graphics_system.h>
#include <rex/input/flags.h>         // guide_button cvar
#include <rex/input/input.h>         // X_INPUT_STATE, X_INPUT_GAMEPAD_GUIDE
#include <rex/input/input_system.h>  // rex::input::InputSystem::GetState
#include <rex/ui/imgui_drawer.h>     // OnCreateDialogs param
#include "renderer/core/nhl_vk_backend.h"
#include "renderer/core/nhl_overlay.h"
#include "renderer/core/nhl_settings.h"
#endif
#include "tools/replay/src/image_dump.h"
#include "tools/replay/src/xtr_player.h"
#include "overall_weights_dump.h"
#include "stick_list_scan.h"
#include "tunable_registry_dump.h"
#include "tunable_runtime.h"
#include "union_device.h"
#include "game_setup.h"
#include "input_map.h"
#include "vpad_script.h"
#include "anim_decode.h"

// Runtime cvar (defined in rexruntime): when true, guest page 0 is
// no-access. NHL Legacy's file-registration code (sub_82705510) walks an
// empty record table as [0 .. 0] step 24 and reads guest address 8 — legal
// on real hardware where low memory is mapped. Unprotect the zero page to
// match console behavior.
REXCVAR_DECLARE(bool, protect_zero);
REXCVAR_DECLARE(bool, scribble_heap);
REXCVAR_DECLARE(bool, vsync);
REXCVAR_DECLARE(bool, gpu_allow_invalid_fetch_constants);
// Audio queue depth (frames buffered before the SDL device callback underruns to
// silence — the audible pop). Default 8 (~43 ms) is thin when the CP thread stalls
// on resolution-scaled swap work at high SSAA and starves audio servicing; a deeper
// queue adds cushion at the cost of a little latency. Clamped 4-64 in the SDK.
REXCVAR_DECLARE(int32_t, audio_maxqframes);
// Resolve readback: download EDRAM resolve results back to guest RAM so later
// texture fetches see the resolved data (e.g. NHL Legacy's runtime-composited,
// recolorable goalie/player equipment maps) instead of stale garbage. Shared
// string cvar ("none"|"fast"|"full"); the beta D3D12 path used "full". The
// Vulkan enable is vulkan_readback_resolve (already declared in <rex/graphics/flags.h>).
REXCVAR_DECLARE(std::string, readback_resolve);
// D3D12 render-target path: "rtv" (fast host path) vs "rov" (rasterizer-
// ordered-views, accurate per-sample EDRAM — handles the in-game scene's
// predicated tiling/overlapping resolves the rtv path renders black).
REXCVAR_DECLARE(std::string, render_target_path_d3d12);
// TEMP black-screen diagnosis: dump the guest PM4 command stream so we can
// count draw packets (DRAW_INDX / DRAW_INDX_2) vs swap-only frames.
REXCVAR_DECLARE(bool, trace_gpu_stream);
REXCVAR_DECLARE(std::string, trace_gpu_prefix);
// Phase 2 enhancement: internal render-resolution (supersampling) scale.
// Xenia's draw_resolution_scale_x/y; 2 => 2x2 the native 1280x720 internal
// buffers for a sharper image. GPU/VRAM cost; auto-clamped to GPU max.
REXCVAR_DECLARE(int32_t, anisotropic_override);
REXCVAR_DECLARE(int32_t, draw_resolution_scale_x);
REXCVAR_DECLARE(int32_t, draw_resolution_scale_y);
// Host window logical size (Xenia heritage cvars). The SDK's SetupPresentation
// creates the window at a hardcoded 1280x720 and has no resize API yet, so
// these may be ignored by 0.8.0 - verified at runtime.
REXCVAR_DECLARE(int32_t, window_width);
REXCVAR_DECLARE(int32_t, window_height);

// SDK display cvars (UI/Window): borderless-fullscreen start + monitor index.
// Both are restart-required (read during SetupPresentation), so the overlay's
// persisted choices are applied here in OnPreSetup.
REXCVAR_DECLARE(bool, fullscreen);
REXCVAR_DECLARE(int32_t, monitor);

// AMD FidelityFX present-time scaler cvars (UI/Presenter) — only defined when the
// SDK is built with REXGLUE_ENABLE_FIDELITYFX=ON (which also defines this guard).
#if defined(REX_HAS_FIDELITYFX_SDK)
REXCVAR_DECLARE(std::string, present_effect);
REXCVAR_DECLARE(double, present_cas_additional_sharpness);
REXCVAR_DECLARE(double, present_fsr_sharpness_reduction);
#endif

// NHL Legacy color-grade post-process cvars (SDK Vulkan command processor).
REXCVAR_DECLARE(bool, present_grade_enable);
REXCVAR_DECLARE(double, present_grade_exposure);
REXCVAR_DECLARE(double, present_grade_contrast);
REXCVAR_DECLARE(double, present_grade_saturation);
REXCVAR_DECLARE(double, present_grade_brightness);
REXCVAR_DECLARE(double, present_grade_temperature);
REXCVAR_DECLARE(double, present_grade_tint);
REXCVAR_DECLARE(double, present_grade_tonemap);

// NHL: bilinear filtering for guest depth/shadow maps (SDK texture cache).
REXCVAR_DECLARE(bool, shadow_filter_linear);
// NHL: extra depth-domain blur passes for guest depth/shadow maps (0..4).
REXCVAR_DECLARE(int32_t, shadow_softness);

// Win32 process-exit primitives (declared directly to avoid pulling <windows.h>
// into this widely-included header). Used only by the replay-mode fast-exit.
#ifdef _WIN32
extern "C" __declspec(dllimport) void* __stdcall GetCurrentProcess();
extern "C" __declspec(dllimport) int __stdcall TerminateProcess(void* handle, unsigned int code);
#endif

namespace nhl::compat {
// Hard process exit that skips destructors and atexit handlers. The diagnostic
// and replay paths use this deliberately: the guest is mid-flight on other
// threads, so an orderly shutdown would deadlock or crash. _exit(2) is the
// POSIX equivalent of TerminateProcess(GetCurrentProcess(), code).
[[noreturn]] inline void HardExit(unsigned int code) {
#ifdef _WIN32
  ::TerminateProcess(::GetCurrentProcess(), code);
  __builtin_unreachable();
#else
  ::_exit(static_cast<int>(code));
#endif
}
}  // namespace nhl::compat

#ifdef NHL_PGO_INSTRUMENT
// PGO instrumentation profile writer (compiler-rt profile runtime, auto-linked by
// -fprofile-generate). Our exit paths hard-exit via TerminateProcess, which skips
// the atexit handler that normally writes the .profraw — so we must flush it by
// hand before terminating. Only declared/called in the instrumented build.
extern "C" int __llvm_profile_write_file(void);
#endif

// Intel hybrid (P/E-core) thread placement — defined in src/cpu_affinity.cpp.
// Pins the process default CPU set to the Performance cores so the recomp's heavy
// threads don't land on E-cores. Env-gated (NHL_PIN_PCORES, default ON); no-op on
// AMD / non-hybrid CPUs. Confined to its own TU so <windows.h> stays out of here.
void rex_pin_pcores_if_hybrid();

class NhllegacyApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<NhllegacyApp>(new NhllegacyApp(ctx, "nhllegacy",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // PERF (Intel hybrid): pin the process to P-cores before any guest/SDK threads
    // spin up. Env-gated (NHL_PIN_PCORES, default ON); no-op on AMD / non-hybrid.
    rex_pin_pcores_if_hybrid();

    // Substitute our renderer for the SDK's default D3D12 backend. The base
    // SetupPresentation() set config.graphics to a stock D3D12GraphicsSystem
    // just before calling this hook; replace it with our subclass, which reuses
    // the SDK guest-GPU front-end and (for now) logs-and-delegates every draw.
#ifdef NHL_HAVE_VULKAN_BACKEND
    // Vulkan is the PRIMARY backend in this build — default it ON so end users get
    // the optimized Vulkan path (and the enhancements overlay) without setting any
    // env vars. NHL_VK_BACKEND_OFF=1 forces the legacy D3D12 path as a fallback for
    // GPUs lacking fragment-shader-interlock (the fsi path the in-game 3D needs).
    if (!std::getenv("NHL_VK_BACKEND") && !std::getenv("NHL_VK_BACKEND_OFF")) {
      nhl::compat::SetEnv("NHL_VK_BACKEND", "1");
    }
#endif
#if defined(__APPLE__)
    // Pixel shader 0x798AC034734C6A98 is NHL's custom edge-detect antialiasing
    // pass: it samples the resolved scene texture at four half-texel diagonal
    // offsets, and where the local contrast exceeds a threshold it normalises an
    // edge direction and blends samples along it. Under MoltenVK that blend
    // branch produces fully saturated (255,255,255) output, which is the crisp
    // white halo seen hugging every player silhouette and stick.
    //
    // It is NOT a tiling, MSAA or resolve problem: the scene texture this pass
    // reads (0x1AF09000) is clean, and all three of its texture fetch constants
    // point at that same correct texture. Skipping just this one shader removes
    // 100% of the halos (measured: 0/148 saturated edge peaks across four
    // frames, versus ~90% with it enabled) and costs only the game's own edge
    // AA, leaving the image slightly more aliased but far closer to correct.
    //
    // The halo only appears over the 3D scene; over the flat 2D menus the same
    // pass is harmless and does sharpen the art. So the shader hash is always
    // registered here, and whether the skip actually fires is decided per frame
    // by the scene detector in nhl_vk_backend.cpp (edge_aa mode Auto). Setting
    // NHL_KEEP_EDGE_AA=1 pins it on everywhere; the overlay exposes the same
    // three-way choice persistently.
    if (!std::getenv("REX_SKIP_PS") && !std::getenv("NHL_KEEP_EDGE_AA")) {
      nhl::compat::SetEnv("REX_SKIP_PS", "798AC034734C6A98");
    }
#ifdef NHL_HAVE_VULKAN_BACKEND
    if (std::getenv("NHL_KEEP_EDGE_AA")) {
      nhl::graphics::SetEdgeAaMode(nhl::graphics::NhlEdgeAaMode::kAlways);
    } else {
      nhl::graphics::SetEdgeAaMode(
          static_cast<nhl::graphics::NhlEdgeAaMode>(nhl::LoadEdgeAaMode(0)));
    }
#endif
#endif
#ifdef NHL_HAVE_VULKAN_BACKEND
    // SPIKE: opt-in env gate to drive the SDK's native Vulkan ROV/EDRAM backend
    // instead of our D3D12 subclass. Phase A is the stock backend (no subclass) —
    // does the recomp boot→menu→gameplay on Vulkan, and how fast? Default (unset)
    // path is unchanged. NHL_VK_RT_PATH overrides the RT path ("rov"|"fsi").
    if (std::getenv("NHL_VK_BACKEND")) {
      // Our subclass (NhlVkGraphicsSystem) installs NhlVkCommandProcessor, which
      // taps IssueSwap/IssueDraw for the [nhl-vk-fps] report. Forwards to the base
      // Vulkan backend otherwise, so rendering matches the stock path.
      config.graphics = std::make_unique<nhl::graphics::NhlVkGraphicsSystem>();
      // NOTE: the Vulkan backend only honors "fsi" (Path::kPixelShaderInterlock —
      // the real per-tile EDRAM path the in-game 3D scene needs). ANY other value
      // (incl. "rov") falls through to kHostRenderTargets (== D3D12 "rtv"), which
      // renders the in-game scene black/missing. So default to fsi, not rov.
      const char* vk_rt = std::getenv("NHL_VK_RT_PATH");
      REXCVAR_SET(render_target_path_vulkan, std::string(vk_rt ? vk_rt : "fsi"));
      // Perf measurement wants the present rate uncapped: vsync is forced on below
      // (console-pacing experiment) and would peg fps at the refresh rate, hiding
      // both headroom and dense-scene drops. NHL_VK_NO_VSYNC=1 frees it.
      if (std::getenv("NHL_VK_NO_VSYNC")) {
        REXCVAR_SET(vsync, false);
        REXLOG_INFO("[nhl-vk-spike] vsync disabled for perf measurement");
      }
      // Goalie/player equipment maps are runtime-composited via EDRAM resolves;
      // without reading the resolve result back to guest RAM, later texture fetches
      // sample stale garbage (green/striped pads). Mirror the beta D3D12 fix
      // (readback_resolve=full + the backend enable) on Vulkan. NHL_VK_NO_READBACK
      // disables it for A/B.
      if (!std::getenv("NHL_VK_NO_READBACK")) {
        // NHL_VK_READBACK_MODE=full|fast|some selects the sync strategy.
        // kFull calls AwaitAllQueueOperationsCompletion() on EVERY readback
        // resolve - a full GPU drain - so the CPU memcpy sees current pixels.
        // kFast/kSome instead double-buffer and read the PREVIOUS frame's copy,
        // which removes the drain entirely but makes the data one frame stale.
        // The drain is the dominant cost in high-draw scenes: menus run 10-14 fps
        // with it and 16-26 fps without.
        const char* rb_mode_env = std::getenv("NHL_VK_READBACK_MODE");
        const std::string rb_mode = rb_mode_env && *rb_mode_env ? rb_mode_env : "full";
        REXCVAR_SET(readback_resolve, rb_mode);
        REXCVAR_SET(vulkan_readback_resolve, true);
        // Size-gated readback (the SDK honors NHL_VK_READBACK_MAX_LEN in the Vulkan
        // resolve path): kFull does a full GPU drain per resolve so the CPU memcpy
        // reads current pixels, but that drain is only NEEDED for the small
        // runtime-composited equipment textures the guest CPU re-reads (helmet ~960
        // KB, goalie ~2.3 MB resolves). The large framebuffer resolves (1280x720x4
        // = ~3.6 MB, ~7/frame) are GPU-consumed for present and never CPU-read, yet
        // were draining the GPU every frame — the dominant CP-thread stall (~1.33x
        // bench, validated live: helmets+goalies correct at 3 MB; both regress below
        // ~1 MB). Default the threshold to 3 MB (keeps every composite, skips only
        // the framebuffers). Override via the env (set to 0 to disable the gate).
        if (!std::getenv("NHL_VK_READBACK_MAX_LEN")) {
          nhl::compat::SetEnv("NHL_VK_READBACK_MAX_LEN", "3000000");
        }
        REXLOG_INFO("[nhl-vk] resolve readback enabled (readback_resolve={}, "
                    "size gate {} B)",
                    rb_mode, std::getenv("NHL_VK_READBACK_MAX_LEN"));
      }
      // Deepen the audio queue on the Vulkan/SSAA path: at >=2x the CP thread
      // stalls on resolution-scaled swap/fence work and starves the guest audio
      // refill under the 60Hz vsync interrupt cadence, draining the SDL queue to
      // silence (pops/crackle). A deeper queue (16 frames, ~85 ms) rides through
      // those pacing hitches. Read once by AudioSystem at setup; clamp 4-64 lives
      // in the SDK. NHL_AUDIO_MAXQFRAMES overrides for latency/robustness A/B.
      {
        int32_t qframes = 16;
        if (const char* e = std::getenv("NHL_AUDIO_MAXQFRAMES"); e && *e) {
          qframes = int32_t(std::strtol(e, nullptr, 10));
        }
        REXCVAR_SET(audio_maxqframes, qframes);
        REXLOG_INFO("[nhl-audio] audio_maxqframes set to {} (deeper queue vs SSAA "
                    "frame-pacing underruns)",
                    qframes);
      }
      // Enable Guide/PS button pass-through so the host can read it
      // (XInputGetStateEx ordinal 100 / SDL mapping) to toggle the enhancements
      // overlay. The guest never reads Guide (the 360 reserved it for the
      // dashboard), so capturing it host-side never conflicts with gameplay.
      REXCVAR_SET(guide_button, true);
      REXLOG_INFO("[nhl-vk-spike] Vulkan backend selected (render_target_path_vulkan={})",
                  vk_rt ? vk_rt : "fsi");
    } else
#endif
    {
#ifdef NHL_HAVE_D3D12_BACKEND
      config.graphics = std::make_unique<nhl::graphics::NhlD3D12GraphicsSystem>();
#else
      // No D3D12 off-Windows. SetupPresentation() already installed the SDK's
      // stock (Vulkan) graphics system, so leave it in place — this is the
      // NHL_VK_BACKEND_OFF path, which loses only our CP subclass's fps tap.
      REXLOG_INFO("[nhl] D3D12 unavailable on this platform; using the SDK's "
                  "stock Vulkan graphics system.");
#endif
    }
    REXCVAR_SET(protect_zero, false);  // see comment at REXCVAR_DECLARE above
    // NHL Legacy reads heap fields it never wrote (sub_82705510 record walk);
    // it relies on allocations being zero-filled like Xenia/Windows provide.
    REXCVAR_SET(scribble_heap, false);
    // Session-7 experiment: the FE script/Flash timeline runs ~5-6x slow with
    // the main loop unthrottled at ~125fps. Pace like a console (60Hz vsync)
    // to test whether the dt divergence is pacing-coupled. (Skip when the Vulkan
    // spike asked for uncapped present via NHL_VK_NO_VSYNC, set above.)
    if (!std::getenv("NHL_VK_NO_VSYNC")) {
      REXCVAR_SET(vsync, true);
    }
    // Xenia warns "Texture fetch constant has 'invalid' type" on the boot
    // screens and renders anyway with this enabled; testing whether the
    // green flash/overlay artifacts come from skipped fetches.
    REXCVAR_SET(gpu_allow_invalid_fetch_constants, true);
    // Accurate EDRAM path (in-game scene renders black under the rtv path).
    REXCVAR_SET(render_target_path_d3d12, std::string("rov"));
    // Black-screen diagnosis: flip to true to dump the guest PM4 stream to
    // gpu_trace/<title>_stream.xtr (~5MB / 25s boot). Parse with
    // tools/parse_gpu_trace.py / trace_resolve_analysis.py. Xenia accepts the
    // same flags and produces the same format (v1) for side-by-side diffs.
    REXCVAR_SET(trace_gpu_stream, false);
    REXCVAR_SET(trace_gpu_prefix, std::string("gpu_trace/"));
    // --- Resolution: native 1080p, lighter GPU load ---
    // The 360 renders at 1280x720. draw_resolution_scale is an integer
    // multiplier (1 => native 720p, 2 => 1440p supersampled), so there is no
    // exact 1080p internal render. Use native 720p internal (much lower GPU
    // cost than the prior 2x2) and target a 1920x1080 window.
    REXCVAR_SET(draw_resolution_scale_x, 1);
    REXCVAR_SET(draw_resolution_scale_y, 1);
    // Force 16x anisotropic filtering (5) rather than the SDK's 4x default (3).
    // The rink is viewed at a very oblique angle, so the ice markings and board
    // textures are exactly the case anisotropy helps, and this device reports
    // maxSamplerAnisotropy 16. Measured no frame-time cost: still 30 fps at
    // 63 draws/frame. NHL_ANISO overrides (-1 = no override, 0..5 = off..16x).
    {
      int32_t aniso = 5;
      if (const char* e = std::getenv("NHL_ANISO"); e && *e) {
        aniso = int32_t(std::strtol(e, nullptr, 10));
      }
      REXCVAR_SET(anisotropic_override, aniso);
    }
    REXCVAR_SET(window_width, 1920);
    REXCVAR_SET(window_height, 1080);

    // Display mode + monitor: apply the persisted overlay choices (and optional
    // dev env overrides) at launch. Both SDK cvars are read once during
    // SetupPresentation, so they must be set before the window is created here.
    {
      bool start_fullscreen = nhl::LoadFullscreen(/*fallback=*/false);
      if (const char* e = std::getenv("NHL_VK_FULLSCREEN"); e && *e) {
        start_fullscreen = (*e != '0');
      }
      if (start_fullscreen) {
        REXCVAR_SET(fullscreen, true);
        REXLOG_INFO("[nhl-display] starting in borderless fullscreen");
      }
      int32_t mon = nhl::LoadMonitor(/*fallback=*/0);
      if (const char* e = std::getenv("NHL_VK_MONITOR"); e && *e) {
        mon = int32_t(std::strtol(e, nullptr, 10));
      }
      if (mon > 0) {
        REXCVAR_SET(monitor, mon);
        REXLOG_INFO("[nhl-display] target monitor index {}", mon);
      }

      // V-Sync: restart-required SDK cvar. Apply the persisted overlay choice;
      // an absent key leaves the SDK default (true) untouched.
      REXCVAR_SET(vsync, nhl::LoadVsync(REXCVAR_GET(vsync)));
    }
#ifdef NHL_HAVE_VULKAN_BACKEND
    // Enhancement A (docs/vulkan-enhancements-kickoff-prompt.md §3.A): internal-
    // resolution supersampling. The SDK renders the guest 1280x720 internally and
    // downsamples to the window; draw_resolution_scale is an integer multiplier
    // (2 => 1440p internal). Env-driven so 1x/2x A/B needs no rebuild. Only
    // meaningful on the Vulkan fsi path; clamp 1..2. Capped at 2x because at 3x/4x
    // the scaled resolve buffer (512 MB * scale_area) exceeds this GPU class's 4 GB
    // maxStorageBufferRange, so CPU-reread equipment composites can't be read back
    // (render black) and reading past 4 GB hard-faults the GPU. Falls back to the
    // default 1 when unset/invalid.
    if (std::getenv("NHL_VK_BACKEND")) {
      // Supersampling scale: the persisted overlay choice (nhl_enhancements.ini)
      // takes effect at launch; NHL_VK_SS env is a dev override. 0 => unset, leave
      // the default 1x. Restart-required (read once at backend init).
      int32_t scale = nhl::LoadSupersampling(/*fallback=*/0);
      if (const char* ss = std::getenv("NHL_VK_SS"); ss && *ss) {
        scale = int32_t(std::strtol(ss, nullptr, 10));
      }
      if (scale >= 1) {
        // Hard cap at 2x (see above). NHL_VK_SS_ALLOW_UNSAFE=1 lifts the cap for
        // developers reproducing the >4 GB scaled-buffer fault; do NOT ship with it.
        int32_t ss_cap = std::getenv("NHL_VK_SS_ALLOW_UNSAFE") ? 4 : 2;
        if (scale > ss_cap) scale = ss_cap;
        REXCVAR_SET(draw_resolution_scale_x, scale);
        REXCVAR_SET(draw_resolution_scale_y, scale);
        REXLOG_INFO("[nhl-vk-ss] internal-resolution supersampling {}x "
                    "(internal {}x{})",
                    scale, 1280 * scale, 720 * scale);
      }

      // FidelityFX present-time scaler (FSR/CAS). Only present when the SDK was
      // built with REXGLUE_ENABLE_FIDELITYFX=ON; the cvars are restart-required,
      // so apply the persisted overlay choices here before SetupPresentation.
#if defined(REX_HAS_FIDELITYFX_SDK)
      {
        const std::string effect = nhl::LoadFfxEffect(/*fallback=*/"bilinear");
        if (effect != "bilinear") {
          REXCVAR_SET(present_effect, effect);
          REXCVAR_SET(present_cas_additional_sharpness,
                      nhl::LoadFfxCasSharpness(REXCVAR_GET(present_cas_additional_sharpness)));
          REXCVAR_SET(present_fsr_sharpness_reduction,
                      nhl::LoadFfxFsrSharpness(REXCVAR_GET(present_fsr_sharpness_reduction)));
          REXLOG_INFO("[nhl-vk-ffx] present effect '{}'", effect);
        }
      }
#endif

      // Color-grade post-process. Hot-reloadable, but the persisted overlay look
      // is applied here so it's live from the first frame. No-op unless enabled.
      if (nhl::LoadGradeEnable(/*fallback=*/false)) {
        REXCVAR_SET(present_grade_enable, true);
        REXCVAR_SET(present_grade_exposure, nhl::LoadGradeExposure(0.0));
        REXCVAR_SET(present_grade_contrast, nhl::LoadGradeContrast(1.0));
        REXCVAR_SET(present_grade_saturation, nhl::LoadGradeSaturation(1.0));
        REXCVAR_SET(present_grade_brightness, nhl::LoadGradeBrightness(0.0));
        REXCVAR_SET(present_grade_temperature, nhl::LoadGradeTemperature(0.0));
        REXCVAR_SET(present_grade_tint, nhl::LoadGradeTint(0.0));
        REXCVAR_SET(present_grade_tonemap, nhl::LoadGradeTonemap(0.0));
        REXLOG_INFO("[nhl-vk-grade] color grade enabled from persisted settings");
      }

      // Soften shadows: restore bilinear filtering on the guest's depth/shadow
      // maps to reduce the hard, pixelated "striped" look up close. Hot-reloadable
      // (read per-draw in the texture cache), so applying the persisted choice here
      // just makes it live from the first frame. NHL_VK_SHADOW_FILTER env overrides.
      {
        bool soften = nhl::LoadShadowFilterLinear(REXCVAR_GET(shadow_filter_linear));
        if (const char* sf = std::getenv("NHL_VK_SHADOW_FILTER"); sf && *sf) {
          soften = (sf[0] == '1' || sf[0] == 't' || sf[0] == 'T');
        }
        REXCVAR_SET(shadow_filter_linear, soften);
        if (soften) {
          REXLOG_INFO("[nhl-vk-shadow] bilinear shadow filtering enabled");
        }

        // Extra depth-domain blur passes (0 = off). Hot-reloadable; NHL_VK_SHADOW_BLUR
        // env overrides the persisted choice.
        int softness = nhl::LoadShadowSoftness(REXCVAR_GET(shadow_softness));
        if (const char* sb = std::getenv("NHL_VK_SHADOW_BLUR"); sb && *sb) {
          softness = int(std::strtol(sb, nullptr, 10));
        }
        if (softness < 0) softness = 0;
        if (softness > 4) softness = 4;
        REXCVAR_SET(shadow_softness, softness);
        if (softness > 0) {
          REXLOG_INFO("[nhl-vk-shadow] shadow softness blur passes = {}", softness);
        }
      }
    }
#endif
    // Opt-in D3D12 debug layer (NHL_BETA_D3D12_DEBUG): must be set before the
    // provider creates the device. Used to get the exact device-removal reason
    // while debugging the beta owned-draw path.
    if (std::getenv("NHL_BETA_D3D12_DEBUG")) {
#ifdef NHL_HAVE_D3D12_BACKEND
      REXCVAR_SET(d3d12_debug, true);
#endif
      REXLOG_INFO("[nhl-beta] D3D12 debug layer enabled via NHL_BETA_D3D12_DEBUG");
    }
    // Opt-in verbose SDK logging (NHL_LOG_LEVEL=debug|trace): surfaces the texture
    // cache's per-texture create/load actions (TextureKey::LogAction) for diagnosing
    // textures that bind but sample zero.
    if (const char* lvl = std::getenv("NHL_LOG_LEVEL")) {
      REXCVAR_SET(log_level, std::string(lvl));
      // The logger tree is already initialized by the time OnPreSetup runs, so the
      // cvar alone is too late — apply the level to all live categories directly.
      rex::SetAllLevels(spdlog::level::from_str(lvl));
      REXLOG_INFO("[nhl-beta] log_level={} via NHL_LOG_LEVEL", lvl);
    }
    // Force the BINDFUL descriptor path under the beta backend. The base
    // D3D12CommandProcessor (which owns root-signature creation) otherwise picks
    // bindless on capable HW (RTX 4080), so ConfigurePipeline hands our owned
    // draw a BINDLESS root signature whose param[5] is a CBV (SystemConstants),
    // not the SharedMemoryAndEdram descriptor table our bindful binding code
    // assumes — SetGraphicsRootDescriptorTable(5) then faults the draw. Bindful
    // makes the base CP, our PipelineCache, and our hand-written binding code
    // agree. Gated on beta so default gameplay keeps the faster bindless path.
    if (const char* be = std::getenv("NHL_BACKEND"); be && std::strcmp(be, "beta") == 0) {
#ifdef NHL_HAVE_D3D12_BACKEND
      REXCVAR_SET(d3d12_bindless, false);
#endif
      REXLOG_INFO("[nhl-beta] forcing bindful descriptor path (d3d12_bindless=false) for beta");
      // Synchronous pipeline creation (0 creation threads) for beta: the owned-draw
      // takeover otherwise hit an async PSO-creation race (device-removal crash, and
      // root-sig/PSO id=201 mismatches) because the async thread read register state
      // after our per-draw MSAA override was restored. Synchronous = built inline,
      // deterministic. Override with NHL_BETA_PSO_SYNC=<n> for experiments.
      int32_t pso_threads = 0;
      if (const char* v = std::getenv("NHL_BETA_PSO_SYNC")) {
        pso_threads = int32_t(std::strtol(v, nullptr, 10));
      }
#ifdef NHL_HAVE_D3D12_BACKEND
      REXCVAR_SET(d3d12_pipeline_creation_threads, pso_threads);
#endif
      REXLOG_INFO("[nhl-beta] d3d12_pipeline_creation_threads = {}", pso_threads);
      // Disable sparse/tiled shared memory for beta (full 512 MB buffer). Under
      // RenderDoc (which forces this off) the textured root sig was built correctly
      // (8 params) and the id=708 race disappeared — the sparse-buffer path appears
      // to perturb the shader binding-population timing. NHL_BETA_TILED=1 to re-enable.
      if (!std::getenv("NHL_BETA_TILED")) {
#ifdef NHL_HAVE_D3D12_BACKEND
        REXCVAR_SET(d3d12_tiled_shared_memory, false);
#endif
        REXLOG_INFO("[nhl-beta] d3d12_tiled_shared_memory = false (full buffer)");
      }
    }
  }

  // End-user installs assembled by tools/packager ship game data in a
  // "game" directory next to the exe and launch with no arguments. Fill the
  // default only when no --game_data_root was provided (cvar empty) so the
  // dev workflow — explicit flag, tools/drive.ps1 — keeps winning.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const std::filesystem::path candidate =
        rex::filesystem::GetExecutableFolder() / "game";
    if (paths.game_data_root.empty()) {
      std::error_code ec;
      if (std::filesystem::is_directory(candidate, ec)) {
        paths.game_data_root = candidate;
      }
    }
#if defined(__APPLE__)
    // First run: if there is still no default.xex anywhere we would look, walk
    // the user through picking their disc image and unpack it, rather than
    // failing with a log line they will never see. No-op once data exists.
    // NHL_NO_SETUP_UI=1 restores the plain "missing data" failure for scripts.
    std::error_code xec;
    const bool have =
        !paths.game_data_root.empty() &&
        std::filesystem::is_regular_file(paths.game_data_root / "default.xex", xec);
    if (!have && !std::getenv("NHL_NO_SETUP_UI")) {
      const std::filesystem::path chosen = nhl::EnsureGameData(
          paths.game_data_root.empty() ? candidate : paths.game_data_root);
      if (chosen.empty()) {
        REXLOG_INFO("[nhl-setup] no game data selected; exiting");
        nhl::compat::HardExit(0);
      }
      paths.game_data_root = chosen;
    }
    nhl::SetActiveGameDataPath(paths.game_data_root);
#endif
  }

  // NHL Legacy probes cache:\ during boot; rexruntime 0.8.0 only mounts
  // game: (and update: when present), and the guest boot path crashes in
  // file/volume bookkeeping (sub_82705510) right after the failed cache
  // opens. Mirror Xenia: back cache: with a writable host directory.
  //
  // Loose-file overlay: NHL Legacy's resource loader probes cache:\<logical
  // path> before falling back to the .big archive under game:\. When the
  // extracted loose-asset tree (game_data_root/_compiled) is present, mount it
  // read-only *beneath* the writable scratch dir via a UnionDevice, so the
  // guest serves loose files (easy replacement) while its own cache writes stay
  // isolated in cache_dir. (The X360 .big files are EA "EB\0\3" — un-reversed,
  // LZX-compressed — so overlaying loose files is how we avoid repacking them.)
  void OnPostSetup() override {
    namespace fs = rex::filesystem;
    const std::filesystem::path cache_dir = cache_root() / "guest";
    std::error_code ec;
    std::filesystem::create_directories(cache_dir, ec);

    const std::filesystem::path loose_root = game_data_root() / "_compiled";
    // Loose .dds texture overrides (converted to .rx2 at load via tdb-rx2-ffi).
    const std::filesystem::path tex_override_root = game_data_root() / "_loose_tex";
    const bool overlaid = std::filesystem::is_directory(loose_root, ec);

    nhllegacy::UnionDevice* union_dev = nullptr;
    std::unique_ptr<fs::Device> device;
    if (overlaid) {
      auto ud = std::make_unique<nhllegacy::UnionDevice>(
          "\\CACHE", cache_dir, loose_root, tex_override_root);
      union_dev = ud.get();  // stays valid after the move into the VFS
      device = std::move(ud);
    } else {
      device = std::make_unique<fs::HostPathDevice>("\\CACHE", cache_dir,
          /*read_only=*/false);
    }

    auto* vfs = runtime()->file_system();
    if (device->Initialize() && vfs->RegisterDevice(std::move(device))) {
      vfs->RegisterSymbolicLink("cache:", "\\CACHE");
      if (overlaid) {
        // The union mount succeeds on the writable upper layer alone, so only claim
        // the loose overlay is active when the read-only lower layer actually
        // initialized — otherwise loose-file overrides silently do nothing.
        if (union_dev && union_dev->lower_active()) {
          // Verified end-to-end: the guest's per-subfile cache:\<path> lookups
          // (rendering/*.rx2, attribdb/*.vlt, fe/*.bin, ...) now resolve to loose
          // files in _compiled via this union, while whole-archive probes
          // (cache:\*.big) miss and fall back to game:\ as before. Texture .rx2
          // with loose .dds overrides under _loose_tex are converted at load.
          REXLOG_INFO("[loose-overlay] mounted {} read-only beneath cache: {}",
                      loose_root.string(), cache_dir.string());
        } else {
          REXLOG_WARN("[loose-overlay] loose layer UNAVAILABLE ({}); cache: is "
                      "writable-only and loose-file overrides will NOT apply",
                      loose_root.string());
        }
      }
    }
  }

#ifdef NHL_HAVE_VULKAN_BACKEND
  // Stand up the in-game enhancements overlay (Vulkan-path only). Called by the
  // SDK from SetupPresentation after the ImGui drawer exists. The dialog is
  // always registered with the drawer so its per-frame OnDraw can poll the
  // controller Guide/PS button (toggle) even while hidden. We hand it two hooks:
  // a controller poll (host-side InputSystem::GetState — catches both Xbox
  // XInput and PlayStation/SDL pads, both mapped to X_INPUT_GAMEPAD_GUIDE) and
  // the Vulkan fps/draws snapshot for the perf HUD.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    if (!std::getenv("NHL_VK_BACKEND")) {
      return;  // enhancements overlay belongs to the Vulkan-fsi enhancement set
    }
    auto poll_pad = [this]() -> nhl::ui::PadState {
      nhl::ui::PadState pad;
      auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system());
      if (!input) {
        return pad;
      }
      float best_mag = 0.0f;
      for (uint32_t user = 0; user < 4; ++user) {
        rex::input::X_INPUT_STATE state{};
        // GetState returns rex::X_RESULT; 0 == X_ERROR_SUCCESS. (The macro itself
        // expands to an unqualified X_RESULT cast that only resolves inside
        // namespace rex, so compare the literal here.)
        if (input->GetState(user, &state) != 0) {
          continue;
        }
        pad.connected = true;
        pad.buttons |= static_cast<uint16_t>(state.gamepad.buttons);
        // Left stick from whichever connected pad is deflected most, so any
        // controller drives the menu.
        const float lx = static_cast<float>(state.gamepad.thumb_lx) / 32767.0f;
        const float ly = static_cast<float>(state.gamepad.thumb_ly) / 32767.0f;
        const float mag = std::fabs(lx) + std::fabs(ly);
        if (mag > best_mag) {
          best_mag = mag;
          pad.lx = lx;
          pad.ly = ly;
        }
      }
      return pad;
    };
    auto perf = []() -> nhl::ui::PerfSnapshot {
      const nhl::graphics::NhlVkPerfSnapshot s = nhl::graphics::ReadVkPerf();
      return nhl::ui::PerfSnapshot{s.fps, s.frame_ms, s.draws_per_frame,
                                   s.frames_total, s.valid};
    };
    // Live display-mode toggle. SetFullscreen does SetWindowLong/SetWindowPos,
    // which must run on the UI (message) thread — marshal via the app context.
    // Also persist so the choice survives a restart (the `fullscreen` cvar is
    // only read at window open).
    auto set_fullscreen = [this](bool fs) {
      app_context().CallInUIThreadDeferred([this, fs]() {
        if (auto* w = window()) {
          w->SetFullscreen(fs);
        }
      });
      nhl::SaveFullscreen(fs);
    };
    auto is_fullscreen = [this]() -> bool {
      auto* w = window();
      return w && w->IsFullscreen();
    };
    auto on_exit = []() {
      // The recomp's normal app/kernel/GPU teardown (QuitFromUIThread -> OnDestroy,
      // which joins the still-running guest thread) HANGS when torn down mid-guest-
      // lifecycle — the trace-replay path documents the same hazard and deliberately
      // hard-exits. A graceful quit here hung on "Exit Game". The game has nothing
      // unsaved at this point, so flush the log and terminate the process directly.
#ifdef NHL_PGO_INSTRUMENT
      __llvm_profile_write_file();  // capture the PGO profile before hard-exit
#endif
      rex::ShutdownLogging();
      nhl::compat::HardExit(0u);
    };
    // Live engine-tunable store (World B). Built lazily on a worker thread when
    // the overlay's "Engine Tunables" section is first opened (the scan is only
    // valid post tweak-registration). The guest memory base is resolved by this
    // provider AT BUILD TIME, not now — OnCreateDialogs runs before the guest
    // memory subsystem is initialized, so touching it here would crash.
    {
      auto vbase_provider = [this]() -> const uint8_t* {
        auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
        return (gs && gs->memory())
                   ? gs->memory()->TranslateVirtual<const uint8_t*>(0u)
                   : nullptr;
      };
      tunable_store_ =
          std::make_unique<nhllegacy::TunableRuntimeStore>(std::move(vbase_provider));
      // If nhl_tunables.json already has saved overrides, kick the build after a
      // registration delay so they re-apply even if the overlay is never opened.
      if (std::filesystem::exists(nhllegacy::TunablesOverridePath())) {
        unsigned delay_ms = 12000;
        if (const char* d = std::getenv("NHL_DUMP_DELAY_MS"); d && *d)
          delay_ms = static_cast<unsigned>(strtoul(d, nullptr, 0));
        auto* store = tunable_store_.get();
        std::thread([store, delay_ms]() {
          ::Sleep(delay_ms);
          store->RequestBuild();  // applies persisted overrides on completion
        }).detach();
      }
    }
    // Developer Tools: scan live guest memory for the create-player stick-picker
    // list (docs/.../editplayer-equipment-enumeration.md). Resolves the guest
    // membase at click time (the guest is running by then) and runs the scan on a
    // detached thread so the present loop never stalls. Writes next to the exe.
    auto dev_scan_stick_list = [this]() {
      auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
      const uint8_t* vbase =
          (gs && gs->memory()) ? gs->memory()->TranslateVirtual<const uint8_t*>(0u)
                               : nullptr;
      if (!vbase) {
        REXLOG_WARN("[stick-scan] no guest memory available");
        return;
      }
      const std::string out =
          (rex::filesystem::GetExecutableFolder() / "stick_list_scan.txt").string();
      std::thread([vbase, out]() {
        REXLOG_INFO("[stick-scan] scanning guest memory -> {}", out);
        nhllegacy::ScanStickList(vbase, out.c_str());
        REXLOG_INFO("[stick-scan] done -> {}", out);
      }).detach();
    };
    enh_overlay_ = std::make_unique<nhl::ui::NhlEnhancementsDialog>(
        drawer, std::move(poll_pad), std::move(perf), std::move(on_exit),
        std::move(set_fullscreen), std::move(is_fullscreen), tunable_store_.get(),
        std::move(dev_scan_stick_list));
    REXLOG_INFO("[nhl-vk] enhancements overlay ready (Guide/PS button or F1 to toggle)");
  }
#endif

  // Replay-and-screenshot mode. With NHL_REPLAY_XTR=<frame.xtr>, replay the
  // captured frame through the live backend + presenter (instead of launching
  // the guest), read back the presented image, and write replay_frame.png next
  // to the exe. Reuses the host's fully set-up window/presenter/command
  // processor, so the resolves + scaler complete — unlike the headless tool,
  // where IssueSwap had no presenter to flush into (black output).
  void LaunchModule() override {
    // Controller remapping, applied in the XamInputGetState override.
    nhllegacy::LoadInputMap();
    // Scripted virtual gamepad (NHL_VPAD_SCRIPT); no-op when unset.
    nhllegacy::StartVpadScript();
    // NHL_SHOT_AFTER_SEC=<n>[,<n>...]: play normally, then capture the presented
    // guest frame to shot_<n>s.png at each listed time. Headless-friendly proof
    // that the renderer is producing real output (macOS screencapture needs
    // Screen Recording permission, which a terminal usually lacks).
    // NHL_SHOT_AFTER_FRAME=<n>[,<n>...]: capture at an exact GUEST FRAME index
    // instead of a wall-clock time. The guest runs a fixed scripted input
    // timeline, so frame N is the same moment in every run - which makes A/B
    // captures between two builds/settings genuinely comparable. Wall-clock
    // captures drift by seconds and land on different scenes.
    if (const char* s = std::getenv("NHL_SHOT_AFTER_FRAME"); s && *s) {
      std::vector<uint64_t> at;
      for (const char* p = s; *p;) {
        char* end = nullptr;
        const uint64_t v = std::strtoull(p, &end, 10);
        if (end == p) break;
        at.push_back(v);
        p = (*end == ',') ? end + 1 : end;
      }
      std::thread([this, at]() {
        for (uint64_t want : at) {
          while (nhl::graphics::ReadVkFrameIndex() < want) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
          }
          char name[64];
          std::snprintf(name, sizeof(name), "frame_%llu.png",
                        static_cast<unsigned long long>(want));
          CaptureFrameToPng(name);
        }
      }).detach();
    }
    if (const char* s = std::getenv("NHL_SHOT_AFTER_SEC"); s && *s) {
      std::vector<unsigned> at;
      for (const char* p = s; *p;) {
        char* end = nullptr;
        const unsigned v = static_cast<unsigned>(std::strtoul(p, &end, 10));
        if (end == p) break;
        at.push_back(v);
        p = (*end == ',') ? end + 1 : end;
      }
      std::thread([this, at]() {
        unsigned elapsed = 0;
        for (unsigned t : at) {
          if (t > elapsed) {
            std::this_thread::sleep_for(std::chrono::seconds(t - elapsed));
            elapsed = t;
          }
          char name[64];
          std::snprintf(name, sizeof(name), "shot_%us.png", t);
          CaptureFrameToPng(name);
          DumpResolveTargets();
        }
      }).detach();
    }
#ifdef NHL_PGO_INSTRUMENT
    // PGO capture helper: with NHL_PGO_DUMP_AFTER=<seconds>, play normally, then
    // auto-flush the profile and hard-exit after that long — makes profile capture
    // scriptable and independent of overlay/Exit-Game navigation. Spawn the timer,
    // then fall through to the normal launch so the game actually runs.
    if (const char* s = std::getenv("NHL_PGO_DUMP_AFTER"); s && *s) {
      const unsigned secs = static_cast<unsigned>(std::strtoul(s, nullptr, 10));
      std::thread([secs]() {
        ::Sleep(secs * 1000u);
        std::fprintf(stderr, "[nhl-pgo] auto-dumping profile after %u s\n", secs);
        __llvm_profile_write_file();
        rex::ShutdownLogging();
        nhl::compat::HardExit(0u);
      }).detach();
    }
#endif
    // Stage B of the Overall-formula hunt (overall_weights_dump.h). With
    // NHL_DUMP_OVERALL_WEIGHTS set, the image is already mapped into guest
    // memory by Setup, so scan it for the gCardOverallWeights_* anchors +
    // weight arrays and fast-exit WITHOUT launching the guest (no window, no
    // game loop). Reuses the NHL_REPLAY_XTR fast-exit discipline below.
    // Runtime variant: let the guest boot so its tweak registration runs, then
    // scan committed guest memory for the now-materialised name->value links
    // and dump the weight floats. Spawn the watcher, then fall through to the
    // normal LaunchModule so the guest actually starts registering.
    if (const char* rt = std::getenv("NHL_DUMP_OVERALL_RUNTIME"); rt && *rt) {
      auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
      const uint8_t* vbase =
          (gs && gs->memory()) ? gs->memory()->TranslateVirtual<const uint8_t*>(0u)
                               : nullptr;
      unsigned delay_ms = 12000;
      if (const char* d = std::getenv("NHL_DUMP_DELAY_MS"); d && *d) {
        delay_ms = static_cast<unsigned>(strtoul(d, nullptr, 0));
      }
      const std::string out =
          (rex::filesystem::GetExecutableFolder() / "overall_weights_runtime.txt")
              .string();
      if (vbase) {
        std::thread([vbase, out, delay_ms]() {
          std::fprintf(stderr, "[ovr-rt] waiting %u ms for tweak registration...\n",
                       delay_ms);
          ::Sleep(delay_ms);
          nhllegacy::DumpOverallWeightsRuntime(vbase, out.c_str());
          rex::ShutdownLogging();
          nhl::compat::HardExit(0u);
        }).detach();
      } else {
        std::fprintf(stderr, "[ovr-rt] no memory base; cannot scan\n");
      }
      rex::ReXApp::LaunchModule();
      return;
    }

    // NHL_ANIM_DECODE: headless animation-decode harness (docs cba-format.md §6).
    // Milestone 0 validates the host->guest call mechanism by invoking the LEAF
    // decode fns (sub_82CE3750 / sub_82CA7330) on a synthetic asset and asserting
    // their outputs — no allocator, no booted guest needed — then fast-exits.
    // Value "1" = log-only; any other value = report-file path. Later milestones
    // (prime/seek/sub_82CA7BD0) will switch to the boot-then-decode pattern below.
    if (const char* ad = std::getenv("NHL_ANIM_DECODE"); ad && *ad) {
      // Milestone 1 recon: NHL_ANIM_DECODE=scan boots the guest, waits for it to
      // load anim.cba, then scans committed guest memory for resident GD records
      // and writes anim_scan_report.txt. Unlike milestone 0 (fast-exit), this must
      // let the guest run — same boot-then-scan pattern as NHL_DUMP_*_RUNTIME.
      if (std::strncmp(ad, "scan", 4) == 0) {
        const uint8_t* vbase =
            runtime()->memory() ? runtime()->memory()->virtual_membase() : nullptr;
        // anim.cba loads on-ice, not at the menu — so auto-fire the scan when the
        // bundle becomes resident (clip-name string appears in heap) rather than
        // racing a fixed delay against the user navigating into a game. Cap the
        // wait (default 5 min; NHL_DUMP_DELAY_MS overrides) and scan anyway on
        // timeout so a report always lands. Get into an on-ice game while it waits.
        unsigned timeout_ms = 300000;
        if (const char* d = std::getenv("NHL_DUMP_DELAY_MS"); d && *d) {
          timeout_ms = static_cast<unsigned>(strtoul(d, nullptr, 0));
        }
        const std::string out =
            (rex::filesystem::GetExecutableFolder() / "anim_scan_report.txt").string();
        if (vbase) {
          std::thread([vbase, out, timeout_ms]() {
            std::fprintf(stderr,
                         "[anim-scan] waiting up to %u ms for anim.cba to load "
                         "(get into an on-ice game)...\n",
                         timeout_ms);
            const bool loaded = nhllegacy::WaitForAnimData(vbase, timeout_ms);
            std::fprintf(stderr, "[anim-scan] %s — scanning now\n",
                         loaded ? "anim data detected" : "TIMEOUT (scanning anyway)");
            nhllegacy::RunAnimScan(vbase, out.c_str());
            rex::ShutdownLogging();
            nhl::compat::HardExit(0u);
          }).detach();
        } else {
          std::fprintf(stderr, "[anim-scan] no membase; cannot scan\n");
        }
        rex::ReXApp::LaunchModule();  // boot the guest so anim.cba loads
        return;
      }
      const bool ok = nhllegacy::RunAnimDecode(runtime()->memory(), ad);
      rex::ShutdownLogging();
      nhl::compat::HardExit(ok ? 0u : 1u);
    }

    // NHL_DUMP_IMAGE: write the decompressed guest image (.rodata/.data/.text,
    // mapped by Setup at 0x82000000) to a raw file, then fast-exit without
    // launching the guest. Lets static tables compiled into the binary (e.g. the
    // create-player equipment picker's fixed stick-ID list) be located offline by
    // scanning the dump, instead of grepping the LZX-compressed default.xex (which
    // finds nothing). Same fast-exit discipline as NHL_DUMP_OVERALL_WEIGHTS below.
    if (const char* di = std::getenv("NHL_DUMP_IMAGE"); di && *di) {
      auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
      if (gs && gs->memory()) {
        constexpr uint32_t kImageBase = 0x82000000u;
        constexpr uint32_t kImageSize = 0x1EA0000u;
        const uint8_t* img =
            gs->memory()->TranslateVirtual<const uint8_t*>(kImageBase);
        const std::string out =
            (rex::filesystem::GetExecutableFolder() / "guest_image_dump.bin").string();
        if (std::FILE* f = std::fopen(out.c_str(), "wb")) {
          std::fwrite(img, 1, kImageSize, f);
          std::fclose(f);
          std::fprintf(stderr, "[img-dump] wrote %u bytes @ %08X -> %s\n",
                       kImageSize, kImageBase, out.c_str());
        } else {
          std::fprintf(stderr, "[img-dump] failed to open %s\n", out.c_str());
        }
      } else {
        std::fprintf(stderr, "[img-dump] no graphics/memory system available\n");
      }
      rex::ShutdownLogging();
      nhl::compat::HardExit(0u);
    }

    if (const char* dump = std::getenv("NHL_DUMP_OVERALL_WEIGHTS"); dump && *dump) {
      auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
      if (gs && gs->memory()) {
        constexpr uint32_t kImageBase = 0x82000000u;
        constexpr uint32_t kImageSize = 0x1EA0000u;
        const uint8_t* img =
            gs->memory()->TranslateVirtual<const uint8_t*>(kImageBase);
        const std::string out =
            (rex::filesystem::GetExecutableFolder() / "overall_weights_dump.txt")
                .string();
        nhllegacy::DumpOverallWeights(img, kImageBase, kImageSize, out.c_str());
      } else {
        std::fprintf(stderr, "[ovr-dump] no graphics/memory system available\n");
      }
      rex::ShutdownLogging();
      nhl::compat::HardExit(0u);
    }

    // NHL_DUMP_TUNABLES: enumerate the engine tweak-registration pool(s) from
    // the mapped image (tunable_registry_dump.h) and fast-exit. Produces the
    // full gXxx tunable catalog — physics / skating / collision / animation /
    // AI / fighting / goalie / rules — by category + name, with any inline
    // defaults. Writes tunable_registry.txt + tunable_registry.json next to the
    // exe. This is the static name catalog; live values come from the RUNTIME
    // variant below (defaults are sparse in the pool, resolved at registration).
    if (const char* dump = std::getenv("NHL_DUMP_TUNABLES"); dump && *dump) {
      auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
      if (gs && gs->memory()) {
        constexpr uint32_t kImageBase = 0x82000000u;
        constexpr uint32_t kImageSize = 0x1EA0000u;
        const uint8_t* img =
            gs->memory()->TranslateVirtual<const uint8_t*>(kImageBase);
        const auto folder = rex::filesystem::GetExecutableFolder();
        const std::string txt = (folder / "tunable_registry.txt").string();
        const std::string json = (folder / "tunable_registry.json").string();
        nhllegacy::DumpTunableRegistry(img, kImageBase, kImageSize, txt.c_str(),
                                       json.c_str());
      } else {
        std::fprintf(stderr, "[tunables] no graphics/memory system available\n");
      }
      rex::ShutdownLogging();
      nhl::compat::HardExit(0u);
    }

    // NHL_DUMP_TUNABLES_RUNTIME: let the guest boot so its tweak registration
    // runs, then scan committed guest memory for the now-materialised
    // name->value records and record each tunable's live value. Writes
    // tunable_values_runtime.txt + .json. NHL_DUMP_DELAY_MS overrides the wait.
    if (const char* rt = std::getenv("NHL_DUMP_TUNABLES_RUNTIME"); rt && *rt) {
      auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
      const uint8_t* vbase =
          (gs && gs->memory()) ? gs->memory()->TranslateVirtual<const uint8_t*>(0u)
                               : nullptr;
      unsigned delay_ms = 12000;
      if (const char* d = std::getenv("NHL_DUMP_DELAY_MS"); d && *d) {
        delay_ms = static_cast<unsigned>(strtoul(d, nullptr, 0));
      }
      const auto folder = rex::filesystem::GetExecutableFolder();
      const std::string txt = (folder / "tunable_values_runtime.txt").string();
      const std::string json = (folder / "tunable_values_runtime.json").string();
      const std::string catalog = (folder / "tunables_catalog.json").string();
      if (vbase) {
        std::thread([vbase, txt, json, catalog, delay_ms]() {
          std::fprintf(stderr, "[tunables-rt] waiting %u ms for tweak registration...\n",
                       delay_ms);
          ::Sleep(delay_ms);
          nhllegacy::DumpTunableValuesRuntime(vbase, txt.c_str(), json.c_str(),
                                              catalog.c_str());
          rex::ShutdownLogging();
          nhl::compat::HardExit(0u);
        }).detach();
      } else {
        std::fprintf(stderr, "[tunables-rt] no memory base; cannot scan\n");
      }
      rex::ReXApp::LaunchModule();
      return;
    }

    const char* xtr = std::getenv("NHL_REPLAY_XTR");
    if (!xtr || !*xtr) {
      rex::ReXApp::LaunchModule();
      return;
    }
    const std::string xtr_path = xtr;
    // NHL_REPLAY_BENCH=<iterations>: deterministic CPU benchmark of the SDK command
    // processor (PM4 decode + state translation + submission) — re-replays the
    // frame N times and logs warm min/median/mean ms. Used to A/B an optimized SDK
    // rebuild (replay bypasses the guest recomp, so it isolates the SDK layer).
    int bench_iters = 0;
    if (const char* b = std::getenv("NHL_REPLAY_BENCH"); b && *b) {
      bench_iters = int(std::strtol(b, nullptr, 10));
    }
    // Defer to the UI thread (presenter fully live), then replay on a background
    // thread (it blocks awaiting the GPU worker), keeping the UI message loop
    // pumping meanwhile.
    app_context().CallInUIThreadDeferred([this, xtr_path, bench_iters]() {
      replay_thread_ = std::thread([this, xtr_path, bench_iters]() {
        const bool ok = bench_iters > 0 ? BenchmarkReplayMode(xtr_path, bench_iters)
                                        : ReplayAndCapture(xtr_path);
        // Replay mode never launched the guest module, so the normal
        // app/kernel/GPU teardown path — which assumes a running guest lifecycle
        // — double-frees and corrupts the heap on exit (0xC0000374 in ntdll,
        // after the artifact is already written). The PNG is on disk; flush the
        // log and terminate the process directly, bypassing the broken teardown.
        // Exit code reflects capture success so automation can detect a failure.
        // (Normal game runs are unaffected: this branch is only taken under
        // NHL_REPLAY_XTR.)
        rex::ShutdownLogging();
        nhl::compat::HardExit(ok ? 0u : 1u);
      });
    });
  }

  void OnShutdown() override {
#ifdef NHL_PGO_INSTRUMENT
    // Safety net: OnShutdown runs at the START of OnDestroy, before the teardown
    // that hangs — so flush the PGO profile here too (covers an X-close exit).
    __llvm_profile_write_file();
#endif
#ifdef NHL_HAVE_VULKAN_BACKEND
    // ReXApp::OnDestroy() calls OnShutdown() BEFORE it resets the ImGui drawer,
    // so destroy our dialog here while the drawer is still alive (the dialog dtor
    // calls drawer->RemoveDialog). Resetting it after teardown would dangle.
    enh_overlay_.reset();
#endif
    // Only reached on the normal (non-replay) path; replay mode fast-exits above.
    if (replay_thread_.joinable()) {
      replay_thread_.join();
    }
  }

  // Other available hooks:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnCreateDialogs(ui::ImGuiDrawer* drawer) override {}

 private:
  // Deterministic CPU benchmark of the SDK command processor (NHL_REPLAY_BENCH).
  // Replays the frame `iters` times and logs warm min/median/mean ms.
  bool BenchmarkReplayMode(const std::string& xtr_path, int iters) {
    auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
    if (!gs) {
      REXLOG_ERROR("[nhl-bench] no graphics system");
      return false;
    }
    const nhl::replay::ReplayStats stats =
        nhl::replay::BenchmarkReplay(*gs, xtr_path, iters);
    if (!stats.ok) {
      REXLOG_ERROR("[nhl-bench] failed: {}", stats.error);
    }
    return stats.ok;
  }

  // Returns true only if the replay produced and wrote the PNG, so the caller can
  // surface a nonzero exit code on failure (CI/automation otherwise sees success).
  bool ReplayAndCapture(const std::string& xtr_path) {
    auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
    if (!gs) {
      REXLOG_ERROR("[nhl-replay] no graphics system");
      return false;
    }
    const nhl::replay::ReplayStats stats = nhl::replay::ReplayTrace(*gs, xtr_path);
    REXLOG_INFO(
        "[nhl-replay] {}: {} draw packets, {} leaf packets, {} resolves, {} frames "
        "(skipped {} wait_reg_mem, {} empty)",
        xtr_path, stats.draw_packets, stats.primary_packets, stats.memory_ranges, stats.frames,
        stats.skipped_wait_reg_mem, stats.skipped_empty_leaves);

    auto* presenter = gs->presenter();
    if (!presenter) {
      REXLOG_ERROR("[nhl-replay] no presenter to capture from");
      return false;
    }
    rex::ui::RawImage img;
    if (!presenter->CaptureGuestOutput(img) || img.width == 0 || img.height == 0) {
      REXLOG_ERROR("[nhl-replay] CaptureGuestOutput failed/empty ({}x{})", img.width, img.height);
      return false;
    }
    // RawImage is R8G8B8X8 with a row stride; repack tight RGBA (opaque).
    std::vector<uint8_t> rgba(static_cast<size_t>(img.width) * img.height * 4);
    for (uint32_t y = 0; y < img.height; ++y) {
      for (uint32_t x = 0; x < img.width; ++x) {
        const uint8_t* s = img.data.data() + static_cast<size_t>(y) * img.stride + x * 4;
        uint8_t* d = rgba.data() + (static_cast<size_t>(y) * img.width + x) * 4;
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = 255;
      }
    }
    const std::string out =
        (rex::filesystem::GetExecutableFolder() / "replay_frame.png").string();
    if (nhl::replay::WritePng(out, img.width, img.height, rgba.data())) {
      REXLOG_INFO("[nhl-replay] wrote {} ({}x{})", out, img.width, img.height);
      return true;
    }
    REXLOG_ERROR("[nhl-replay] failed to write {}", out);
    return false;
  }

  // NHL_DUMP_RESOLVE=<hex addr>[,<hex addr>]  NHL_DUMP_RESOLVE_AT=<seconds>
  // Dumps a resolve DESTINATION straight out of guest memory as a PNG,
  // un-tiling it with the SDK's Xenos 2D tiled-address function. This is the
  // ground truth the macOS EDRAM-fold investigation was missing: it shows what
  // each individual resolve actually produced, separating the two per-pass
  // resolves from the final 1280x720 "un-fold" resolve.
  void DumpResolveTargets() {
    const char* list = std::getenv("NHL_DUMP_RESOLVE");
    if (!list || !*list) return;
    auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
    if (!gs || !gs->memory()) {
      REXLOG_ERROR("[nhl-dumpres] no memory system");
      return;
    }
    // Entries are <hex addr>[:<width>x<height>], comma separated. The
    // post-process chain renders at 640x360, so the size has to be per-entry.
    std::vector<uint8_t> rgba;
    for (const char* p = list; *p;) {
      char* end = nullptr;
      const uint32_t addr = uint32_t(std::strtoul(p, &end, 16));
      if (end == p) break;
      uint32_t kW = 1280, kH = 720;
      if (*end == ':') {
        kW = uint32_t(std::strtoul(end + 1, &end, 10));
        if (*end == 'x') kH = uint32_t(std::strtoul(end + 1, &end, 10));
      }
      p = (*end == ',') ? end + 1 : end;
      if (!kW || !kH) continue;
      rgba.assign(size_t(kW) * kH * 4, 0);
      const uint8_t* base = gs->memory()->TranslatePhysical<const uint8_t*>(addr);
      if (!base) {
        REXLOG_ERROR("[nhl-dumpres] cannot translate 0x{:08X}", addr);
        continue;
      }
      uint64_t sum = 0;
      for (uint32_t y = 0; y < kH; ++y) {
        for (uint32_t x = 0; x < kW; ++x) {
          const int32_t off = rex::graphics::texture_util::GetTiledOffset2D(
              int32_t(x), int32_t(y), kW, /*bytes_per_block_log2=*/2);
          const uint8_t* src = base + off;
          uint8_t* dst = rgba.data() + (size_t(y) * kW + x) * 4;
          // Channel order varies with the destination format and endian swap, so
          // make it selectable: NHL_DUMP_SWIZZLE=<3 byte indices>, e.g. "123"
          // for ARGB or "210" for BGRA (the previous hard-coded behaviour).
          static const char* sw = std::getenv("NHL_DUMP_SWIZZLE");
          const int i0 = sw && sw[0] ? sw[0] - '0' : 1;
          const int i1 = sw && sw[1] ? sw[1] - '0' : 2;
          const int i2 = sw && sw[2] ? sw[2] - '0' : 3;
          dst[0] = src[i0 & 3];
          dst[1] = src[i1 & 3];
          dst[2] = src[i2 & 3];
          dst[3] = 255;
          sum += (unsigned(dst[0]) + dst[1] + dst[2]) / 3u;
        }
      }
      {
        // Log a mid-frame pixel's raw bytes so the real layout is identifiable.
        const int32_t off_mid = rex::graphics::texture_util::GetTiledOffset2D(
            int32_t(kW / 2), int32_t(kH / 2), kW, 2);
        const uint8_t* m = base + off_mid;
        REXLOG_INFO("[nhl-dumpres] 0x{:08X} {}x{} centre raw bytes = {:02X} {:02X} {:02X} {:02X}",
                    addr, kW, kH, m[0], m[1], m[2], m[3]);
      }
      char name[64];
      std::snprintf(name, sizeof(name), "resolve_%08X.png", addr);
      const std::string out = (rex::filesystem::GetExecutableFolder() / name).string();
      if (nhl::replay::WritePng(out, kW, kH, rgba.data())) {
        REXLOG_INFO("[nhl-dumpres] wrote {} luma_mean={:.1f}", out,
                    double(sum) / (double(kW) * kH));
      }
    }
  }

  // Captures the currently presented guest frame to <exe dir>/<filename>.
  // Same RawImage -> tight RGBA repack as the replay screenshot path.
  bool CaptureFrameToPng(const char* filename) {
    auto* gs = static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
    auto* presenter = gs ? gs->presenter() : nullptr;
    if (!presenter) {
      REXLOG_ERROR("[nhl-shot] no presenter available");
      return false;
    }
    rex::ui::RawImage img;
    if (!presenter->CaptureGuestOutput(img) || img.width == 0 || img.height == 0) {
      REXLOG_ERROR("[nhl-shot] CaptureGuestOutput failed/empty ({}x{})", img.width, img.height);
      return false;
    }
    std::vector<uint8_t> rgba(static_cast<size_t>(img.width) * img.height * 4);
    // Also compute a coarse "is this actually a picture?" signal, so the log
    // alone distinguishes a real frame from an all-black or uniform buffer.
    uint64_t sum = 0;
    uint8_t lo = 255, hi = 0;
    for (uint32_t y = 0; y < img.height; ++y) {
      for (uint32_t x = 0; x < img.width; ++x) {
        const uint8_t* s = img.data.data() + static_cast<size_t>(y) * img.stride + x * 4;
        uint8_t* d = rgba.data() + (static_cast<size_t>(y) * img.width + x) * 4;
        d[0] = s[0];
        d[1] = s[1];
        d[2] = s[2];
        d[3] = 255;
        const uint8_t lum = static_cast<uint8_t>((s[0] + s[1] + s[2]) / 3);
        sum += lum;
        lo = std::min(lo, lum);
        hi = std::max(hi, lum);
      }
    }
    const double mean = double(sum) / (double(img.width) * img.height);
    const std::string out = (rex::filesystem::GetExecutableFolder() / filename).string();
    if (!nhl::replay::WritePng(out, img.width, img.height, rgba.data())) {
      REXLOG_ERROR("[nhl-shot] failed to write {}", out);
      return false;
    }
    REXLOG_INFO("[nhl-shot] wrote {} ({}x{}) luma mean={:.1f} min={} max={}", out, img.width,
                img.height, mean, lo, hi);
    return true;
  }

  std::thread replay_thread_;
#ifdef NHL_HAVE_VULKAN_BACKEND
  // Live engine-tunable store backing the overlay's "Engine Tunables" section.
  // Declared BEFORE the dialog so it is destroyed AFTER it (members destruct in
  // reverse declaration order) — the dialog holds a raw pointer into it.
  std::unique_ptr<nhllegacy::TunableRuntimeStore> tunable_store_;
  // In-game enhancements overlay (created in OnCreateDialogs under NHL_VK_BACKEND,
  // destroyed in OnShutdown before the SDK tears down the ImGui drawer).
  std::unique_ptr<nhl::ui::NhlEnhancementsDialog> enh_overlay_;
#endif
};
