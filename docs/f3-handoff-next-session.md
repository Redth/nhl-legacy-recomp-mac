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

### F-4 first step — BUILT (2026-06-22), pending a dense controller run
The decisive probe is implemented + built clean as **`NHL_HIGHCUT_BUSYPROBE=1`**
(`nhl_command_processor.cpp`, the per-60-frame `[highcut-perf]` window report). It reads the **CP
thread's own CPU time** (`GetThreadTimes` on the thread that runs BOTH the SDK PM4 decode AND
`RenderBetaOwnedDraw`, then blocks at the coexistence sync) and splits the frame wall into:
- **CPU-busy** (`busyPct`, ms/frame) = SDK decode + our work (the thread actually executing)
- **blocked** (`wall − busy`, ms/frame) = the thread idle on coexistence (GPU wait / producer-consumer stall)
- and, with `NHL_HIGHCUT_PROFILE` on (it is, in the recipe), a sub-line splitting CPU-busy into
  **our `RenderBetaOwnedDraw`** vs **SDK-PM4-decode(+other)**.

**Decision rule (printed inline in the log):**
- `busy << wall` (large `blocked` ms/frame) ⇒ the ~31 ms is coexistence GPU-WAIT ⇒ **F-4 reclaims it
  ~for free** (≈2× fps). Proceed with F-4 takeover.
- `busy ~= wall` (`blocked ≈ 0`, busy 90–100%) ⇒ the ~31 ms is SDK PM4 decode (CPU) ⇒ **F-4 does not
  free it**; re-plan toward draw-level caching / attacking our 34 ms.

**Run it:** `scripts\_f4busyprobe.ps1` (full live recipe + the flag; pointed at `win-amd64-vk-ffx`).
**YOU must drive into dense gameplay and HOLD ~30s+**, then close the game — it greps the
`F4-busyprobe` lines. Autonomous smoke test (attract, 3–7 draws/frame, ~450 fps) already confirmed the
probe FIRES and doesn't crash, reporting `CPU-busy 56–70%, blocked ~0.8 ms/frame` — but that is light
scene data, **NOT** the decisive dense number (the ~31 ms outside-cost only appears at ~1500 draws).

### F-4 first-step RESULT (2026-06-22, dense controller run — DECISIVE, OVERTURNS the F-4 bet)
Dense gameplay measured (~10 windows, 1356–1606 draws/frame, ~14–19 fps, log `nhllegacy_058.log`):

| bucket | per frame | share |
|---|---|---|
| **CP-thread CPU-busy** | **~59 ms (90–95%)** | wall is CPU |
| — our `RenderBetaOwnedDraw` | ~34 ms | 53% |
| — SDK PM4 decode (+base render/present) | ~25 ms | 39% |
| **blocked (coexistence GPU wait)** | **~5 ms** | **8%** |
| frame wall | ~64 ms | — |

**Verdict: `busy ~= wall` (90–95% busy, blocked ≈ 5 ms) ⇒ the ~31 ms "outside" is ~25 ms CPU + only
~5 ms GPU wait — NOT "mostly coexistence GPU-WAIT."** The handoff's F-4 bet ("mostly wait ⇒ F-4
removes it ~free ⇒ ~2× fps") is **REFUTED**: F-4's pure-wait win is ~5 ms (~8%), ~15→~16 fps. The
frame is **CPU-bound on the CP thread**; removing GPU contention barely moves it.

**Reframed path to 60 fps (both levers are CPU, not GPU):**
- **Draw-level caching** — attacks our ~34 ms (53%), fully ours, now the single biggest lever. BUT
  floors at ~30 ms/frame (~33 fps): the SDK still walks the full PM4 every frame (~25 ms), which our
  caching cannot skip (the guest re-emits all draws; we only cache *our* post-decode processing).
- **SDK-decode reduction (the real F-4)** — the ~25 ms is the SDK CP decoding all PM4 + its base
  render/present *CPU* submission. F-4 ("headless") only helps if it removes the throwaway-base-render
  CPU work; the geometry-decode part is needed. **Unknown: how the ~25 ms splits between needed-decode
  vs throwaway-base-render.** That split is the next F-4 measurement and F-4 is **no longer cheap**.
- The per-draw SDK owned-draw render is ALREADY skipped (`skip_owned_render`, NHL_HIGHCUT_LIVE_FEED),
  so the ~25 ms is the residual SDK front-end + frame-boundary work.

### F-5 DRAW-LEVEL CACHE — BUILT + mechanism validated (2026-06-23), pending dense controller run
Chosen lever (per the F-4 result: attack our ~34 ms, the biggest CPU slice). `NHL_HIGHCUT_DRAWCACHE=1`
(live-feed only), in `RenderBetaOwnedDraw`: cache the fully-serialized live packet and, on an unchanged
draw, re-push it while skipping translate-tail/gap1/untile/gap2/serialize (~25 of the 34 ms).
- **KEY = a single CONTENT+STATE hash, deliberately ADDRESS-FREE.** v1 keyed on buffer addresses and
  thrashed (guest ring-buffers vtx/idx every frame → 76,798 entries / 222 MB / **1% hit**). v2 hashes
  what the packet encodes — shader ids, geometry/texture CONTENT, float constants, render state — never
  a guest pointer. Double-buffered by guest-present (promote `s_drawNext`→`s_drawPrev` each frame) so it
  holds ~one frame of draws and evicts stale dynamic draws automatically.
- **Autonomous attract validation: 1% → 73–91% hit, entries 76,798 → ~130, mem ~0 MB, no crash/errors.**
  That's the light menu scene; the dense-gameplay hit rate + fps gain is the decisive number and needs
  the controller.
- **STALENESS (accepted choice):** texture content keyed on the SAME 512 B prefix the untile cache uses
  (moving player's bone-palette texture changes in its first 512 B → miss → reprocessed; **players that
  move stay live**). Vtx/idx content keyed on a 512 B prefix by default — small NEW staleness on
  same-address content changes past 512 B; `NHL_HIGHCUT_DRAWCACHE_HASHLEN=0` hashes FULL vtx/idx content
  (zero added staleness, slower) for A/B.
- **Run:** `scripts\_drawcache.ps1` (live recipe + drawcache + busyprobe + profile). Drive into dense
  gameplay, hold ~30 s, close. Compare `draw cache: …% hit`, `live takeover: N fps` (vs ~15 baseline),
  and `F4-busyprobe our RenderBetaOwnedDraw=…ms` (should drop from ~34) against `scripts\_f4busyprobe.ps1`
  (no cache). **Verify players/puck animate** (not frozen). Expected floor even at 100% hit ≈ ~33 fps
  (the ~25 ms SDK decode survives — that's the F-4 follow-on).
- Code: `nhl_command_processor.cpp` — cache block at the top of the `p3_dump_data` heavy block
  (~:2173), store on miss before `HighcutLivePushDraw`, report by the untile-cache line. NOT committed.

#### F-5 dense controller RESULT (2026-06-23) — NEGATIVE: whole-packet caching fails in gameplay
Log `nhllegacy_061`. Dense (~700–1530 draws): **hit rate 3–13%** (vs 73–91% on the menu), **fps
unchanged (~14–21 = baseline)**, `our RenderBetaOwnedDraw` still ~24–39 ms. **Root cause = the moving
broadcast camera:** every draw multiplies by the per-frame view/projection matrix carried in its VS
float constants, so every packet differs frame-to-frame → near-zero hits. The rink is static in WORLD
space but NOT in PACKET space once the camera moves — so the handoff premise ("most of a 1500-draw scene
is static") was geometrically true but false for byte-identical packets. Only camera-independent 2D
HUD/scoreboard (~77 draws) hits. Cache itself is correct + bounded (no crash, 0 MB) — the workload
defeats whole-packet granularity. **DRAWCACHE is a menu-only win; keep it gated/off for gameplay.**

**Corrected lever → cache the camera-independent BULK at finer grain.** The expensive
camera-independent work is the vertex/index GATHER+COPY (gap1 ~13 ms + packet ~6 ms; untile is already
content-cached). Recommended: **stream vertex/index buffers BY-ID (content-hashed) like shaders+textures
already are** (the `HighcutLivePushResource` infra exists; geometry is the last INLINED bulk in the
packet). Then the per-frame packet is small (floats+state+ids) → cheap to rebuild every frame even with
a moving camera, and geometry copies once per unique mesh.

### F-5 BY-ID GEOMETRY — BUILT (2026-06-23), pending dense controller run + VISUAL check
`NHL_HIGHCUT_GEOM_BYID=1` (live only). Packet **v12**: added `vtx_id`/`idx_id` to `DrawPacketHeader`.
- **Producer** (`nhl_command_processor.cpp`): the vertex gather is now two-pass — rebase the fetch
  offsets + hash the concatenated stream content in place (read, no copy) → `vtx_id` (domain-tagged);
  build the `shared_blob` COPY only on first sight (`s_sentGeo`), so later frames skip the gather copy.
  Index buffer the same (`idx_id`). In by-id mode `shared_bytes`/`index_bytes` = 0 (geometry not inlined)
  and the blobs are streamed once via `HighcutLivePushResource` (alongside the existing shader/tex stream).
- **Consumer** (`plume_present.cpp`): when `shared_bytes==0 && vtx_id` (and idx) resolve the bytes from
  `c.resourceBytes` (the streamed dict) — `sharedN`/`idxN` are the effective sizes. No staleness: the id
  is a FULL content hash, so changed geometry → new id → re-streamed.
- **Why this beats whole-packet caching:** geometry content is camera/animation-independent (verts are
  bind-pose; transform+skinning are in the VS), so it streams once and stays cached even as the camera
  moves — exactly the property whole-packet caching lacked.
- **Autonomous attract smoke test: no crash, no VUID/validation errors, renders at 250+ fps, draws flow.**
  Structurally sound — but attract is light; the dense producer win and **VISUAL correctness are the
  decisive checks and need the controller.**
- **Run:** `scripts\_drawcache.ps1` (now defaults GEOM_BYID **on**, DRAWCACHE off; `-DrawCache` to add it).
  Drive into dense gameplay, hold ~30 s, close. **(1) Perf:** `window cost` gap1+packet should drop, `our
  RenderBetaOwnedDraw` < the ~34 ms baseline, fps > ~15. **(2) VISUAL — critical:** confirm players/rink/
  puck render CORRECTLY (not exploded/missing) — by-id geometry has correctness risk (id collision, the
  rebased `fetch_blob` vs streamed blob, consumer resolve). If geometry is wrong, the prime suspects are
  the two-pass rebase or a vtx_id collision; A/B by unsetting `NHL_HIGHCUT_GEOM_BYID`.
- NOT committed. (Alt if needed = producer-only partial cache: reuse gathered vtx blob + tex descs by
  content, rebuild only floats + serialize.)

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
