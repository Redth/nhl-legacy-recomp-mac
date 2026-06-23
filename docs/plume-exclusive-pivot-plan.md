# Pivot plan — divorce from EDRAM/FSI: exclusive plume renderer (D3D9-hook frame graph + PM4 geometry tap)

> **Status:** PLAN (decided to pursue 2026-06-22). Reverses the 2026-06-16 "SDK Vulkan FSI is the
> baseline" decision ([current-status.md](current-status.md), [vulkan-migration-plan.md](vulkan-migration-plan.md))
> in favor of a renderer we fully own, with **no EDRAM, no fold, no fragment-shader-interlock**.
>
> **North star:** plume is the *sole* renderer and the *sole* presenter. rexglue does CPU recomp +
> headless PM4 geometry decode only — it never produces or shows a pixel.

---

## 0. The non-negotiable target: *exclusively* plume

A prior misunderstanding had plume running **alongside** rexglue's EDRAM render (both active,
plume in its own window). That coexistence was **bring-up scaffolding only** — every Path C
milestone was built additive + env-gated so it could be verified without disturbing the working
game. **It is not the goal.**

The goal is **exclusive plume**:

| Component | Endstate |
|---|---|
| rexglue CPU recomp (PPC→native game logic) | **KEPT** — this is the game; nothing replaces it |
| rexglue PM4 **decode front-end** (parse, primitive processor, register decode, Xenos→SPIR-V) | **KEPT, but headless** — used only to extract geometry/state from inlined draws; renders nothing |
| rexglue GPU **render backend** (EDRAM emulation, FSI/host-RT, render-target cache, resolve) | **OFF** — deleted from the live path |
| rexglue **present** (SDK presenter / VdSwap path) | **OFF** — plume presents |
| **plume** (our Vulkan renderer) | **THE renderer + THE present** — only source of pixels |

If at the end of this pivot rexglue's GPU backend is still drawing anything, the pivot is not done.

---

## 1. The governing constraint (verified, immovable)

NHL Legacy's **per-draw `DrawIndexedPrimitive` packet emission is INLINED** into the recompiled
code — confirmed by the live M1 runtime tap, not a static guess
([phase0-d3d9-hookability.md](phase0-d3d9-hookability.md) §"M1 UPDATE",
[highcut-h1-resource-graph.md](highcut-h1-resource-graph.md)). EA's compiler inlined the draw path
into raw command-buffer (PM4) stores ~15 years ago; that function boundary does not exist in the
binary.

**Consequences:**

- **PM4 cannot be eliminated for this title.** There is no higher-level draw call to intercept.
  For NHL Legacy, *the draws are the PM4 stream*. No recompiler change recovers what the original
  compiler inlined away.
- **An Unleashed/XenonRecomp-style toolchain swap is a trap here.** That approach works for titles
  whose draws are out-of-line D3D9 calls (Sonic Unleashed); it carries **no EDRAM/PM4 model**
  ([[unleashed-recompiled-no-edram]]), so for this title's inlined draws it would have *nothing to
  hook for geometry*. rexglue's PM4 decode is what makes this title tractable. **Keep rexglue's
  CPU recomp + PM4 decode.**

**But PM4 is not what causes the pain.** EDRAM, the fold, and FSI come from *modeling the
render-target/resolve memory as Xenos EDRAM* — not from the draws. So the pivot does not delete
PM4; it **demotes** PM4 from "the EDRAM substrate" to "a thin geometry channel."

---

## 2. Why the divorce is reachable: the D3D9 frame graph is fold-free

H-1 proved that **every render-target/resolve/present signal is available out-of-line at the D3D9
hook level, at TRUE logical size — never the 640-pitch fold**
([highcut-h1-resource-graph.md](highcut-h1-resource-graph.md)). The fold is purely an artifact of
the PM4/EDRAM *emulation* one layer below; at the D3D9 interception level it does not exist.

| signal | D3D9 hook | logical value |
|---|---|---|
| present surface | `sub_827F1C88` | 1280×720 |
| viewport extent | `sub_827E6480` | 1280×720 |
| resolve dest | `sub_827EF8E0` (count-exact = Resolve) | 1280×720 |
| sampled textures | `sub_827E5938` | true sizes |

So the architecture is a **hybrid high cut**:

```
                D3D9 HOOKS (out-of-line, logical, fold-free)
  resource-create ─┐
  SetViewport ─────┤→  flat plume RTs @ logical size   ┐
  Resolve ─────────┤→  host-copy RT→texture            ├─→  PLUME (sole renderer + present)
  Present ─────────┘→  drive plume swapchain           │
                                                        │
                PM4 GEOMETRY TAP (inlined draws only)   │
  decode verts/indices/shaders/constants ──────────────┘  (no EDRAM semantics; just triangles)
```

EDRAM never exists. The fold never happens. FSI is never needed. PM4 is reduced to a payload of
"here are some triangles, with this shader and these constants, into the currently-bound logical
RT."

---

## 3. What is already built (do not rebuild)

| Piece | Where | State |
|---|---|---|
| plume in-process device + swapchain, driven by guest Present | `gpu/hooks/plume_present.cpp` (H-2) | built |
| Xenos ucode → SPIR-V translator (ported, spirv-val-clean) | `gpu/spirv/*` (Path C P-1…P-3) | built |
| Flat-RT render of real decoded draws: geometry, textures, depth/stencil, blend, multi-draw frame | Path C C-3…C-5c, `plume_present.cpp` | built (menu ~90% faithful; 3D depth build-clean, runtime-verify pending) |
| **D3D9 logical resource graph** (resource/RT/resolve/present at logical size) | `gpu/hooks/d3d9_resources.cpp` (H-1, `NHL_HIGHCUT`) | built, runs live |
| PM4 draw decode (verts/indices/constants/viewport/topology) | `renderer/core/nhl_command_processor.cpp::RenderBetaOwnedDraw` | built |

The two halves — **the flat plume renderer (Path C)** and **the fold-free D3D9 frame graph
(H-1)** — exist but were **never fused**. Path C sizes its RTs from PM4-inferred EDRAM state; H-1
observes the true logical graph but drives nothing. The pivot is the fusion + the finish + the
takeover.

> Caveat: `gpu/hooks/d3d9_resources.cpp` and `gpu/hooks/d3d9_tap.cpp` override the same guest
> symbols, so only one is in `NHLLEGACY_SOURCES` at a time. The fused path needs the resource hooks
> live concurrently with the CP decode — reconcile the source-set/weak-alias arrangement in F-1.

---

## 4. Milestones

Each milestone has a binary exit criterion. F-1…F-3 build the fused renderer; F-4 makes it
exclusive; F-5 reaches parity; F-6 is the gating performance proof.

### F-1 — Flat RTs sized from the D3D9 graph (kills the fold structurally)
Replace Path C's PM4-inferred surface sizing with H-1's D3D9-hooked logical sizes. Each guest
color/depth surface (observed via `CreateRenderTarget`/`CreateTexture` + the bound viewport) → one
flat plume RT at logical size. Reconcile the `d3d9_resources.cpp` ↔ CP-decode source arrangement so
both run together.
**Exit:** a live frame's RTs are all created at logical size from D3D9 hooks (no 640-pitch anywhere
in the plume path); the existing menu render still composites correctly into them.

**FINDINGS (2026-06-22, F-1.1/F-1.2 done):**
- **Coexistence PROVEN (F-1.1).** `d3d9_resources.cpp` (NHL_HIGHCUT) runs concurrently with the
  beta D3D12 capture (NHL_HIGHCUT_FRAME_CAPTURE) in one process — both log streams present; D3D9
  graph reports present/viewport/resolves all at logical 1280×720. No source-arrangement conflict.
- **The premise was partly off — the replay is ALREADY fold-free in *sizing*.** `LoadC5Frames`
  sizes each offscreen surface RT from the per-draw **logical viewport** (`vpW/vpH`,
  `plume_present.cpp:1421-1427`), not the EDRAM pitch. The 640-pitch survives ONLY as a component
  of the `SurfaceKey` *identity*, never as a dimension. So F-1 is smaller than written: it's not
  "stop EDRAM-pitch sizing" (already done) but **(a) remove the EDRAM-tile values from the surface
  *identity key*, and (b) replace the viewport-max size heuristic** — which has documented failure
  modes (C-5h "camera sunk in the ice": a half-res aux pass out-areas the real view) — **with the
  D3D9 graph's authoritative per-resource logical size.**
- **Bridge primitive landed (F-1.2, built clean).** `HighcutD3D9FrameSize()` +
  `HighcutD3D9LogicalSizeByBase(base_addr,…)` in `d3d9_resources.cpp` expose the logical graph
  (frame size; resource logical size by decoded GPU base) for the capture/replay pipeline to size +
  key from.
- **F-1.3 DONE — address correlation is a DEAD END (decisive).** Validated on a live dense frame
  (auto-latched 505 draws / 352 3D, `[F1-corr]`/`[F1-reg]` logging):
  - **Frame/primary size correlates RELIABLY** (D3D9 present = capture = 1280×720) — because it comes
    straight from the Present args, not object decode.
  - **Per-surface correlation FAILS — 0 of 315 resolves matched.** Diagnosis (`HighcutD3D9DumpRegistry`):
    the PM4 resolve dests are **EDRAM-resolved physical addresses (`0x1A0xxxxx`)**, but the D3D9
    registry's decoded bases are **main-memory GPU addresses (`0xF0xxxxxx`)** — *different address
    spaces*. (Also: the H-1 `DecodeDims` width/height layout, validated on one full-screen texture,
    yields garbage for arbitrary object types — `4098×1` etc. — so the registry decode is unreliable
    beyond present/viewport.) **`copy_dest_base ↔ Resource.baseAddr` is the wrong bridge.**
- **CONCLUSION / reframe:** the correct way to attribute a draw/surface to a D3D9 logical RT is
  **binding-order** (track the currently-bound RT/viewport at draw time), NOT after-the-fact address
  matching — and that is exactly what the **live F-3** path must do anyway. Since the capture/replay is
  **already fold-free** (logical viewport sizing) and the *reliable* D3D9 signal (frame size) already
  matches, **F-1's fold goal is effectively met**; per-surface D3D9-authoritative sizing/keying should
  **fold into F-3 (live binding-order)** rather than be forced through the dead address bridge here.
  - **Kept:** `HighcutD3D9FrameSize()` (reliable, useful as the primary/swapchain authority).
  - **Known-not-useful:** `HighcutD3D9LogicalSizeByBase()` + the `[F1-corr]`/`[F1-reg]` diagnostics
    (one-shot, gated on capture+NHL_HIGHCUT — harmless in the shipped path; remove when convenient).
  - **Net:** F-1 reduced from "swap per-surface authority to D3D9" → "fold goal already satisfied;
    per-surface D3D9 authority moves to F-3." Recommend proceeding to F-2/F-3 rather than chasing the
    address bridge.

### F-2 — Resolve = host-copy (render-to-texture)
Drive host-copy of a flat plume RT → a plume texture so shadow maps, reflections, and post-process
composite correctly without EDRAM resolve.
**Exit:** a 3D scene's shadow-map / reflection / post chain composites correctly from resolves; no
SDK EDRAM resolve involved.

**FINDINGS (2026-06-22) — already met on the offline path; the "D3D9-hooked" framing is the same
dead bridge as F-1.** Examined the live dense capture replayed through plume:
- **The resolve=host-copy mechanism already exists and is wired, PM4-keyed** (C-5d.3:
  `ParseResolveGraphBytes` → `resolveMap[dest_addr]` → `GetOrCreateSurfaceRT` → rebind stubbed
  bindings). Replay loads the resolve graph (9 dest mappings / 12 markers), renders clean (0 VUID).
  It re-points stubbed depth/shadow bindings (shown: 66 depth rebinds on a prior frame; 0 on this
  frame because its shadow dests were captured real via readback-resolve C-5l). **No SDK EDRAM
  resolve is involved — already host-copy.**
- **Fold-free confirmed on a REAL fold surface.** This frame's PRIMARY surface is
  `pitch=640 msaa=1` (1280-wide rendered into a 640-pitch EDRAM surface at 2×) — the canonical fold
  case — and the replay sizes it logically (viewport 1280×720) and renders it correctly.
- **The "D3D9-hooked resolve" reframe (keyed by the D3D9 resolve dest) hits F-1.3's wall:** the
  D3D9 resolve dest is a different address space than the PM4 `copy_dest_base` the host-copy keys on.
  And the PM4 resolve capture (`HighcutCaptureResolve`) already tracks the same `sub_827EF8E0`
  resolves count-exact — the D3D9 hook adds nothing on the offline path.
- **CONCLUSION:** like F-1, F-2's goal is **already satisfied on the offline path** via the PM4-keyed
  mechanism; the D3D9-hook version is inherently **F-3 (live)** work — there, the D3D9 resolve hook
  *triggers* a live host-copy and there's no PM4-vs-D3D9 correlation to reconcile.

### F-3 — Draw-tap → flat render (live)  ← **detailed scope: [f3-live-draw-tap-plan.md](f3-live-draw-tap-plan.md)**
Replace the static disk replay with a live per-frame feed so plume renders the running game. **Scoped
2026-06-22 — and it's mostly already built:** the C-6 live feed (`NHL_HIGHCUT_LIVE_FEED`) is wired
end-to-end (CP `HighcutLivePushDraw`/`HighcutLiveCommitFrame` → in-memory bridge → plume rebuild),
with by-ID GPU-object caches; it hit 22–60 fps on the menu. Surface attribution is **already
binding-order** (per-draw PM4 surface registers — not D3D9 address, per F-1.3). So F-3 = a **perf +
live-gameplay-correctness** push, not a build: F-3.1 baseline+profile live gameplay → F-3.2 consumer
incremental rebuild (the named "next increment") → F-3.3 producer dirty-tracking + kill the by-value
copy → F-3.4 live resolve=host-copy → F-3.5 dense gameplay correctness+framerate.
**Exit:** live gameplay renders correctly through plume at real-time (coexisting with rexglue).

### F-4 — TAKEOVER: make plume exclusive  ← *the goal*
Suppress rexglue's GPU render backend and present so plume is the only output:
- rexglue GPU backend produces nothing (no EDRAM, no FSI, no SDK render-target cache, no SDK
  present). Investigate running rexglue's CP decode **headless** (decode without a render-target
  backend attached) — or, if that proves entangled, decode the tapped PM4 ourselves.
- plume presents to the real game window (not a secondary window).
**Exit:** with rexglue's render backend disabled, the game is fully playable and visually faithful
with **plume as the sole renderer and presenter**. No coexistence.

### F-5 — Parity pass
Sweep correctness across all scenes (menu, intro 3D, create-player, gameplay, replay). The FSI
baseline reached parity with 4 small fixes; the owned path needs its own sweep (lighting/gamma,
jersey numbers/names, equipment normals + cube reflections, alpha-to-coverage net, MSAA).
**Exit:** user-verified parity with the retired FSI baseline across all scenes.

### F-6 — Performance  ← ~~the gating risk~~ **DE-RISKED (2026-06-22 spike): PROVEN**
Path C's old ~3 fps was **entirely bring-up artifacts** (per-frame CPU untile, disk I/O,
dual-GPU coexistence) — F-3/F-4 remove all three. **Measured spike result** (RTX 4080 SUPER;
`NHL_HIGHCUT_PERF` instrumentation in `plume_present.cpp::RenderClear`, validation layer OFF):

- **Captured frame:** the attract-demo auto-latched a dense **500-draw / 346-3D** broadcast frame
  (real on-ice broadcast presentation — representative of gameplay density).
- **Replay through plume, steady state (WITH rexglue coexistence contending for the GPU):**
  CPU-record ~0.37 ms, **GPU ~5.7 ms, frame ~6.0 ms → ~165 fps.**
- **Tail of the run, once rexglue's GPU work dropped away (≈ the F-4 exclusive endstate):**
  **GPU ~1.0 ms, frame ~1.5 ms → ~650–700 fps.** Same 500 draws — so the ~5.7 ms steady-state was
  ~5× inflated by *coexistence contention*, exactly the overhead F-4 removes.

**Verdict: owned-plume renders a dense 3D frame at 2.7× real-time even in the unoptimized,
contended bring-up state, and ~11× when it has the GPU to itself.** CPU command-buffer build for
500 draws is trivial (~0.4 ms). The pivot is not perf-gated. The make-or-break risk is retired.

**Caveats / remaining confirmation:** the spike is a STATIC replay (resources untiled/uploaded/
pipelined ONCE at load), so it measures GPU render cost + CPU record cost, **not** the live
per-frame upload/translate cost F-3 adds (dynamic geometry re-upload, new-draw shader/pipeline
build). Those are cacheable (pipeline-by-hash, texture-by-address, only dynamic vfetch ranges
re-upload — see [[highcut-live-takeover-freeze-fix]]) and start from a 0.4 ms base, so headroom is
large — but a live F-3 measurement is the final confirmation. Also: high-end GPU; weaker hardware
scales down, but a ~1 ms exclusive cost leaves margin for an 8×-slower GPU to clear 60 fps.
**Levers if ever needed:** GPU-compute untile, descriptor/buffer pooling, MT command recording.

---

## 5. Risks / open decisions

- **F-6 perf is a genuine unknown.** Recommend an early de-risking spike (a representative dense
  frame through the fused live path, measured) before committing the full F-2…F-5 build-out. If
  plume can't approach real-time, the whole pivot is moot and the FSI baseline should be kept.
- **Headless rexglue CP decode (F-4).** Whether the SDK CP front-end can run with no render-target
  backend attached is unverified. Fallback: parse the tapped PM4 ourselves (more work, fully
  decouples from the SDK GPU).
- **Loss of the FSI "free" wins.** This re-opens everything [vulkan-migration-plan.md](vulkan-migration-plan.md)
  marked SUBSUMED. The owned path must re-earn each via the F-5 parity sweep.
- **Hook source-set conflict (F-1).** `d3d9_resources.cpp` vs `d3d9_tap.cpp` override the same
  symbols; the fused build needs them coexisting — resolve the weak-alias/source arrangement.

## 6. What rexglue is reduced to (the endstate, restated)

1. CPU recomp — the game's logic (PPC→native). Untouched.
2. A **headless PM4 geometry decoder** for the inlined draws — produces no pixels.

Everything that sizes a render target, resolves, draws a triangle, or presents a frame is **plume,
exclusively**.
