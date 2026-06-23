# Next-session handoff — EDRAM-divorce pivot, F-3 (live draw-tap) perf

> Read this first, then [plume-exclusive-pivot-plan.md](plume-exclusive-pivot-plan.md) (master plan)
> + [f3-live-draw-tap-plan.md](f3-live-draw-tap-plan.md) (F-3 detail). Branch: `perf/cpu-draw-submission`.

## You are here (1 paragraph)

We are pivoting the renderer to **plume, EDRAM-free** (no fold/FSI), with rexglue reduced to CPU
recomp + a headless geometry decoder. Feasibility is **proven** (F-6 spike). The **live path renders
real gameplay** through plume today (F-3.0 done). Two measured perf passes (F-3.2 untile, F-3.3
packet) took dense live gameplay from ~5.7 → **~15–20 fps**. The remaining ~65 ms/frame is **two
roughly-equal halves** (F-3.4 finding): ~34 ms of *our* per-draw work + ~31 ms of *SDK decode +
coexistence GPU wait*. **Neither lever alone reaches 60 fps.** Correctness has known gaps (deferred
to F-5). Nothing is blocked on feasibility — it's now perf + completeness engineering.

## The decision waiting for you (pick one)

| Lever | Attacks | Cost / risk | Note |
|---|---|---|---|
| **Draw-level caching** | our ~34 ms (skip re-processing unchanged static draws) | medium; **worsens staleness** | most of a 1500-draw scene is static rink/boards/UI |
| **F-4 (coexistence removal)** ← *recommended* | the ~31 ms **+ it's the endgame** (plume sole renderer) **+ kills the 2nd window** | larger milestone; doesn't worsen correctness | turn rexglue's GPU/present off |

**Recommended: start F-4.** First concrete F-4 step is cheap and decisive — **measure whether the
~31 ms is coexistence GPU-WAIT (removable) or SDK PM4 decode (needed)**: bracket the whole
`RenderBetaOwnedDraw` vs CP idle, or A/B with the base render suppressed. If it's mostly wait, F-4
removes it ~for free and roughly doubles fps; if it's SDK decode, re-plan.

## Reproduce the live dense measurement (NEEDS THE USER AT THE CONTROLLER)

Autonomous attract runs DON'T reach dense gameplay (the live feed slows the game so the attract loop
sits on light menus, ≤14 draws/frame). **A human must drive into a game and HOLD there.**

Build: `scripts\_build_highcut_ffx.bat` (builds `nhllegacy` in `out/build/win-amd64-vk-ffx`; the old
`win-amd64-relwithdebinfo` beta dir was deleted in the 2026-06-17 consolidation — don't use
`_build_beta.bat`).

Run (single process; ALL these env vars; user drives into gameplay during the window):
```
NHL_VK_BACKEND_OFF=1   (force the D3D12 backend — beta takeover needs it; build auto-forces VK otherwise)
NHL_BACKEND=beta NHL_BETA_TAKEOVER=1 NHL_BETA_LIVE=1 NHL_BETA_FLAT=1 NHL_BETA_DEPTH=1
NHL_BETA_LIVE_START_FRAME=200          (defer takeover past boot)
NHL_HIGHCUT_PRESENT=1 NHL_HIGHCUT_C5=1
NHL_HIGHCUT_LIVE_FEED=1 NHL_HIGHCUT_FRAME_CAPTURE=1   (BOTH required — see gotcha)
NHL_HIGHCUT_PROFILE=1                   (producer translate/untile/packet/gap1/gap2 + consumer rebuild)
NHL_HIGHCUT_PERF=1                      (plume render cost; optional)
```
Read the `[highcut-perf] window cost:` lines (now split translate/untile/packet/gap1/gap2) and
`live takeover: N fps`. `_vknet.ps1`-style launch + 180s wait + grep is the pattern (see this
session's PowerShell runs).

## GOTCHAS that cost time this session (don't repeat)

1. **`NHL_HIGHCUT_LIVE_FEED` requires `NHL_HIGHCUT_FRAME_CAPTURE`.** The live build/push path is gated
   on `frame_capture` (`nhl_command_processor.cpp:1686` and `:2733`). live_feed alone = silent no-op
   (0 draws reach plume). **Footgun TODO: OR `live_feed` into those two gates so it works standalone.**
2. **Do NOT hide the plume window (`SW_HIDE`)** — it STALLS the game on the initial screen (never
   progresses; stuck ≤14 draws). Reverted (a4209f8). The 2nd window is inherent to coexistence; it
   goes away properly at F-4.
3. **Beta takeover needs the D3D12 backend** → set `NHL_VK_BACKEND_OFF=1` (the build auto-forces
   `NHL_VK_BACKEND=1`, `nhllegacy_app.h:164`).
4. **Dense gameplay = user-driven only** (point above). The auto-latch (3D-density) only works for the
   disk-CAPTURE path, not the live perf path.
5. **Log rotation** — early lines (e.g. "LIVE takeover ACTIVE at frame 200") rotate out of long runs;
   don't conclude "never happened" from a tail grep.

## Perf state (dense live gameplay, ~1500 draws, ~15 fps, ~65 ms/frame)

- IN `RenderBetaOwnedDraw` (~34 ms): translate ~8, **gap1 (xlat→untile) ~11 (biggest)**, untile ~7
  (100% cache hit — F-3.2 holding), packet ~4, gap2 ~3.
- OUTSIDE it (~31 ms): SDK front-end PM4 decode + coexistence GPU wait.
- consumer rebuild ~44 ms but on the PLUME thread (parallel, 0.03 ms/draw — fine); render ~8 ms (fine).
- If attacking our half incrementally instead of draw-level caching: **gap1** (texture-binding /
  vfetch setup, `nhl_command_processor.cpp` ~lines 1965–2540) is the biggest single in-function bucket.

## Correctness backlog → F-5 parity pass (user-observed; EXPECTED at this phase)

- **Staleness / "players don't update unless they move"** — the untile + by-ID caches decide
  "changed?" from only the **first 512 bytes** of a texture (`nhl_command_processor.cpp:2437`); dynamic
  content (skinned players, bone palettes, animated tex) whose change is past 512 B serves STALE data.
  Tunable: hash more/all, skip-cache dynamic slots, or use guest write-watch. **Draw-level caching
  would worsen this** — factor in when choosing the lever.
- **Crowd not rendered** — almost certainly **instancing**: replay draws 1 instance (`drawInstanced(n,1,…)`);
  crowds are 1 mesh × many instances. Replicate instances.
- **Menu elements partial** — the producer only captures "interesting" draws (vfetch VS or textured
  PS), skipping trivial UI quads + line-list dividers (`:1687`); line lists draw degenerate.
- **Lighting / texture parity** — cube reflections + some PS modes not at parity; exotic tex formats →
  magenta placeholder; swizzle/endian best-effort. (Known items: jersey numbers, equipment tint.)

## Map

- **Plume renderer + live bridge:** `gpu/hooks/plume_present.cpp` (`RenderClear`, `LoadC5Frames`,
  `BuildRenderableDraw`, by-id caches `shaderCache/texCache/pipelineCache`, bridge
  `HighcutLivePushDraw`/`…CommitFrame`, `NHL_HIGHCUT_PERF` instrumentation).
- **CP decode + live push:** `renderer/core/nhl_command_processor.cpp::RenderBetaOwnedDraw` (translate,
  untile cache `s_texCache`+`HcBlob`, packet build, push) + `IssueDraw` takeover activation (`:5641`).
- **D3D9 logical graph (H-1, reference):** `gpu/hooks/d3d9_resources.cpp` — fold-free RT graph; address
  bridge to PM4 is a DEAD END (F-1.3), so it's for F-4-era live binding-order, not offline sizing.
- **Packet format:** `gpu/hooks/highcut_draw_packet.h`.

## Commits this session (branch `perf/cpu-draw-submission`)

`8b90211` pivot plan + F-6 spike + F-1/F-2 · `98b3d1e` F-3.0/3.1/3.2 diag · `63fcb3b` F-3.2 shared_ptr
fix · `fdd58df` F-3.3 packet-move (+ bad hide-window) · `a4209f8` revert hide-window · `11eb827`
F-3.4 subdivision · (+ doc commits). All isolated to my files; the branch's pre-existing work is
untouched.
