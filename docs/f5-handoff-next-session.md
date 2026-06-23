# Next-session handoff — EDRAM-divorce pivot, post-F-5 (perf reality + the parallel-producer spike)

> Read this FIRST. Supersedes [f3-handoff-next-session.md](f3-handoff-next-session.md) (still valid for
> F-1…F-4-first-step detail). Master plan: [plume-exclusive-pivot-plan.md](plume-exclusive-pivot-plan.md).
> Branch: `perf/cpu-draw-submission`. Last commit: **`dfcbe46`** (F-4 busyprobe + F-5 geometry-by-id v12).

## You are here (1 paragraph)

The EDRAM-free plume path **renders live dense gameplay correctly** (user-confirmed: players render
right; remaining flicker/texture gaps are PRE-EXISTING parity items for F-5-parity, not regressions).
Performance is **~15–16 fps dense (~1500 draws, ~64 ms/frame)**. We proved this session the frame is
**CPU-bound on the single CP thread** (~92% busy, only ~5 ms/frame GPU-wait), split ~34 ms our
per-draw extraction + ~25 ms SDK PM4 decode. The **renderer is NOT the bottleneck** (plume render ~8 ms
contended / ~1 ms exclusive; F-6 spike hit 165–650 fps). Two perf passes landed (whole-packet draw cache
= menu-only, gated off; by-id geometry streaming = shipped, correct, but ~flat on fps because the
per-frame hash-read replaced the copy). **The wall is the per-draw CPU FEED, single-threaded.**

## The decision waiting for you — the DECISIVE spike

**Does the per-draw producer extraction (~34 ms) parallelize off the CP thread?** This single result
determines whether 60 fps is on the table. Everything else (F-4, micro-opts) is secondary until this is
answered.

- **The work** (`RenderBetaOwnedDraw`: translate / gap1 tex-binding+gather / untile / packet) is largely
  per-draw-independent → in principle parallelizable across ~8 cores → ~34 ms could drop to ~5–8 ms,
  leaving the ~25 ms SDK decode as the wall (~30 ms/frame → ~33 fps), then F-4 trims further.
- **The risk the spike must surface:** the work reads SHARED MUTABLE SDK state that the SDK rewrites
  between draws — `register_file_` (the 0x4000/0x4800/0x2000 blocks we hash+copy), `beta_current_vs_`,
  and the static producer caches (`s_texCache`, the `HcXlatEntry` translate cache, `s_sentGeo`/
  `s_sentRes`). To move the work off-thread you must (a) SNAPSHOT each draw's inputs on the CP thread
  (copy the needed register ranges + buffer pointers + shader handles), (b) hand to a worker pool, (c)
  make the caches thread-safe (shard/lock). **The snapshot copy (~register ranges × 1500 draws) could
  itself eat the win** — that's exactly what the spike measures.
- **Cheap decisive form:** on the CP thread, snapshot just the inputs translate+untile+packet need into
  a per-draw struct and push to a pool; do the work there; keep the CP thread to (SDK decode + snapshot
  + enqueue). Measure the CP-thread wall (the existing `NHL_HIGHCUT_BUSYPROBE` already reports it). If it
  drops toward ~25–30 ms → parallelization works → pursue it. If snapshot/sync overhead keeps it ~flat →
  parallelization is blocked by the shared-state coupling → 60 fps needs the leaner-custom-decoder path.

## Performance reality (manage expectations)

- **Likely reachable** (MT producer + F-4 + gap1/untile micro-opts): **~30–35 fps dense**, higher on
  lighter scenes.
- **60 fps**: possible but NOT demonstrated — needs the parallel-producer win AND probably replacing the
  SDK CP front-end with a leaner custom PM4 decoder (the big rewrite in F-4's risks). Treat as a research
  goal, not a commitment.
- **CPU-bound ⇒ scales DOWN on weaker hardware** (the 4080S has GPU to spare; the bottleneck is single-
  thread CPU). Portability risk the FSI path didn't have.
- **Context:** the retired FSI/EDRAM baseline ran **66–84 fps**. The plume path inserted a whole
  extract→bridge→rebuild pipeline the baseline never paid (the ~34 ms). The bet: claw perf back AND gain
  the no-EDRAM/fold/FSI enhancement ceiling. The render headroom validates half the bet; the feed cost is
  the open half.

## The levers (with honest ceilings)

| Lever | Attacks | Ceiling | State |
|---|---|---|---|
| **Parallel producer** ← *do the spike first* | our ~34 ms → ~5–8 ms | ~33 fps (then SDK-decode-bound) | UNTRIED — decisive |
| F-4 exclusive plume | ~5 ms GPU-wait + some SDK base-render CPU; kills 2nd window | trims ~5–15 ms; the endgame | first-step done (busyprobe); needs the needed-decode-vs-throwaway-base-render split measured |
| gap1 micro-opt | the ~13 ms tex-binding/vfetch setup (biggest in-fn bucket) | ~5–8 ms saved | not started |
| Leaner custom PM4 decoder | the ~25 ms SDK decode (sequential, needed) | the only path PAST ~33 fps | big rewrite; F-4 fallback |
| Crowd instancing | correctness + draw count | minor perf | F-5-parity |

## Reproduce the dense measurement (NEEDS THE USER AT THE CONTROLLER)

Autonomous attract is light (≤~140 draws); dense gameplay needs a human driving + holding. Build:
`scripts\_build_highcut_ffx.bat` (target `nhllegacy`, dir `out/build/win-amd64-vk-ffx`).

- **`scripts\_f4busyprobe.ps1`** — baseline live recipe + `NHL_HIGHCUT_BUSYPROBE` (CPU-busy vs blocked).
- **`scripts\_drawcache.ps1`** — adds `NHL_HIGHCUT_GEOM_BYID` (shipped lever); `-DrawCache` adds the
  menu-only whole-packet cache; `_drawcache.ps1 0` = full-hash (zero added staleness).
- Read `[highcut-perf]` lines: `live takeover: N fps`, `window cost:` (translate/untile/packet/gap1/gap2),
  `F4-busyprobe:` (CPU-busy% + blocked + our-RenderBetaOwnedDraw-vs-SDK-decode split).

Drive into a game, hold ~30 s, close; the script greps the lines.

## GOTCHAS (don't re-pay these)

1. **`NHL_HIGHCUT_LIVE_FEED` requires `NHL_HIGHCUT_FRAME_CAPTURE`** (gated at `nhl_command_processor.cpp`
   :1686/:2733). live_feed alone = silent no-op. (Footgun TODO: OR live_feed into those gates.)
2. **Beta takeover needs the D3D12 backend** → `NHL_VK_BACKEND_OFF=1` (build auto-forces VK).
3. **Don't SW_HIDE the plume window** — stalls the game on the initial screen. The 2nd window is inherent
   to coexistence; F-4 removes it (plume → real game window).
4. **Dense gameplay = controller only.** Autonomous attract sits on light menus.
5. **Build script prints a benign `vswhere.exe` warning** — ignore; vcvars still initializes. `BUILD_EXIT=0`
   is the truth.
6. **The diagnostic `_*.ps1` scripts are gitignored** (`/scripts/_*.ps1`); force-add (`git add -f`) like
   `_vknet.ps1`/`_highcut.ps1`.

## Code map

- **Producer (CP decode → packet):** `renderer/core/nhl_command_processor.cpp::RenderBetaOwnedDraw`
  (~:1405). Per-draw flow: translate (by-id cached) → vertex/index gather (now **2-pass by-id**, hashes
  content → `vtx_id`/`idx_id`, builds copy only on first sight) → untile (`s_texCache`, 100% hit) →
  hdr build → frame-boundary commit (~:2750, present-count) → serialize+push (`HighcutLivePushDraw`).
  Perf instrumentation + `NHL_HIGHCUT_BUSYPROBE` in the per-60-frame window report.
- **Whole-packet draw cache (menu-only, gated off):** top of the `p3_dump_data` heavy block (~:2173);
  content+state hash, double-buffered by present (`s_drawPrev`/`s_drawNext`).
- **By-id geometry (shipped):** gather rebases fetch offsets + hashes in place; streams blobs via
  `HighcutLivePushResource`; consumer resolves from `c.resourceBytes`. Packet **v12**
  (`gpu/hooks/highcut_draw_packet.h`: `vtx_id`/`idx_id`).
- **Consumer (plume render):** `gpu/hooks/plume_present.cpp::BuildRenderableDraw` (~:826) + by-id caches
  (`shaderCache`/`texCache`/`resourceBytes`); `LoadC5Frames`/`RenderClear` rebuild + present.
- **D3D9 logical graph (reference, dead-end bridge per F-1.3):** `gpu/hooks/d3d9_resources.cpp`.

## This session's flags + commits

Flags (all opt-in, default behavior unchanged): `NHL_HIGHCUT_BUSYPROBE` (F-4 probe),
`NHL_HIGHCUT_DRAWCACHE`[`_HASHLEN`] (menu-only, leave off), `NHL_HIGHCUT_GEOM_BYID` (shipped lever).
Scripts: `_f4busyprobe.ps1`, `_drawcache.ps1`. Commit: **`dfcbe46`** on `perf/cpu-draw-submission`
(6 files, +495). Nothing else pending; the branch's pre-existing work is untouched.
