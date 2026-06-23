# F-3 scope — live draw-tap → plume (real-time live gameplay)

> Sub-plan of [plume-exclusive-pivot-plan.md](plume-exclusive-pivot-plan.md). F-3 is the step that
> replaces the static disk capture/replay with a **live per-frame feed**, so plume renders the
> *actual running game* (not a frozen captured frame) in real time. After F-3, plume mirrors live
> gameplay; F-4 then suppresses rexglue's GPU so plume is the *only* output.

## Goal & exit

- **Goal:** the live game's inlined PM4 draws are decoded and rendered through plume **every frame,
  in real time**, while rexglue still runs (coexistence). EDRAM-free by construction (the F-6 spike
  already proved the render model; F-1/F-2 proved fold-free sizing + host-copy resolve).
- **Exit:** live **gameplay** (not just the menu) renders correctly through plume at real-time
  framerate (target ≥60 fps, accepting coexistence overhead that F-4 removes) — players, puck, ice,
  shadows, HUD. Verified live.

## The big finding: F-3 is mostly already built (C-6 live feed)

This is NOT greenfield. The live bridge exists end-to-end:

| Piece | Where | State |
|---|---|---|
| CP pushes each owned draw's packet | `nhl_command_processor.cpp:2904` `HighcutLivePushDraw` | built |
| CP streams shaders/textures once (by-ID dictionary) | `HighcutLivePushResource` (`plume_present.cpp:2248`) | built |
| CP commits the frame at the present boundary | `nhl_command_processor.cpp:2771` `HighcutLiveCommitFrame` | built |
| In-memory bridge (seq-bumped) | `plume_present.cpp:373-387` (`g_livePendingDraws`, `g_liveSeq`, `g_resourcePending`) | built |
| Consumer rebuilds on new seq | `plume_present.cpp:1586-1601` (RenderClear live block) | built |
| **By-ID GPU-object caches** (persist across frames) | `plume_present.cpp:315-322` (`shaderCache`/`texCache`/`layoutCache`/`pipelineCache`/`resourceBytes`) | built |
| Live resolves carried over the bridge | `HighcutLiveCommitFrame(resolves,…)` | built |

Prior live result ([[highcut-live-takeover-freeze-fix]]): producer caches got it to **22–60 fps on
the menu**; the open bottleneck was the **per-frame packet by-value copy** + the **full consumer
rebuild**. So F-3 is a **perf + live-gameplay-correctness** push on an existing path, not a build
from scratch.

## Two things already solved (don't redo)

1. **Surface attribution is already binding-order.** Each packet carries
   `surface_color_base/depth_base/pitch/msaa` (`highcut_draw_packet.h:196-200`) = the surface bound
   *at draw time* (the CP reads the current surface registers as it walks each draw). That IS the
   binding-order attribution; no new tracking needed. (And per F-1.3, this is the right bridge —
   NOT D3D9-address correlation.)
2. **The render itself is cheap.** F-6 spike: ~1 ms exclusive / ~6 ms contended for 500 draws. So
   the live wall is the **per-frame CPU rebuild/decode/copy**, not plume's GPU work.

## The perf problem, precisely

Per frame, two full passes run regardless of what actually changed:

- **Producer (CP, beta D3D12 thread):** decode every owned draw + build a full packet + push it
  by-value over the bridge. Most draws (rink, boards, crowd, static UI) are **identical every
  frame**; only players/puck/dynamic-UI change. The by-value packet copy of thousands of unchanged
  draws/frame is the named bottleneck.
- **Consumer (plume thread):** `LoadC5Frames` (`plume_present.cpp:1353-1394`) iterates **all** draws
  every frame and `BuildRenderableDraw`s each — even though GPU objects (shaders/textures/pipelines)
  are cached by-id, the per-draw packet parse + RenderableDraw struct build + constant/buffer
  uploads + descriptor-set wiring re-run for every draw. The `[highcut-perf] consumer rebuild: N
  draws in X ms` log (`:1392`) measures exactly this.

**The fix is incremental rebuild on both sides** — process only what changed, reuse the rest.

## Milestones

### F-3.1 — Baseline live gameplay + locate the real bottleneck
Run the live feed on live **gameplay** (not menu) and measure where the per-frame time goes:
producer decode, packet build+copy, bridge, consumer rebuild, plume render (the `NHL_HIGHCUT_PERF`
instrumentation from the spike + the `[highcut-perf] consumer rebuild` log). Menu was 22–60 fps;
gameplay is denser — get the real numbers and the split.
**Run recipe (single process — bridge is in-memory):** `NHL_VK_BACKEND_OFF=1 NHL_BACKEND=beta
NHL_BETA_TAKEOVER=1 NHL_BETA_LIVE=1 NHL_BETA_FLAT=1 NHL_BETA_DEPTH=1 NHL_HIGHCUT_PRESENT=1
NHL_HIGHCUT_C5=1 NHL_HIGHCUT_LIVE_FEED=1` (+ `NHL_HIGHCUT_PERF=1`). Drive into a live game.
**Exit:** a per-stage cost breakdown for a dense gameplay frame; the dominant cost identified.

### F-3.2 — Consumer incremental rebuild (the named "next increment")
Cache `RenderableDraw`s across frames keyed by a stable per-draw identity; only rebuild draws whose
packet changed (the GPU objects are already cached by-id — this caches the *assembled draw*). Skip
re-uploading constants/buffers for unchanged static draws. Target: per-frame consumer cost ∝ the
number of *changed* draws, not total draws.
**Exit:** `[highcut-perf] consumer rebuild` time on a static gameplay view drops toward ~0; only
dynamic draws cost anything.

### F-3.3 — Producer dirty-tracking + drop the by-value copy
On the CP side, push **only changed draws** (static rink/boards/crowd pushed once and marked
persistent; players/puck/dynamic-UI re-pushed per frame), and move packets without a by-value vector
copy (pointer/arena hand-off, or a persistent per-slot packet buffer). This attacks the named
by-value-copy bottleneck at the source.
**Exit:** producer per-frame cost ∝ changed draws; the by-value copy no longer dominates.

### F-3.4 — Live resolve = host-copy (verify, fed live)
Resolves already ride the bridge (`HighcutLiveCommitFrame`). Confirm the F-2 host-copy composite
(shadow maps / reflections) works **live** (it's verified offline; this is feed-it-live + confirm).
**Exit:** live self-shadowing / RTT composites correctly through the live feed.

### F-3.5 — Dense gameplay correctness + framerate
Full live gameplay through plume: players (skinned, dynamic verts re-uploaded each frame), puck,
ice, boards, shadows, HUD — all correct, at real-time. Carry over known correctness items to re-check
on the live path (jersey numbers [[highcut-c5g-jersey-number-residency]], equipment tint
[[highcut-green-equipment-is-capture-residency]]) — several were *capture/takeover-residency*
artifacts that may resolve on a true live feed.
**Exit:** live gameplay is faithful + real-time through plume (coexisting with rexglue). The live
EDRAM-free renderer is proven. → hand to F-4 (exclusive takeover).

## Backend-host decision (F-3 vs F-4)

The live feed is hosted on the **beta D3D12 CP** (`RenderBetaOwnedDraw` decodes draws). F-3 keeps it
there (where the feed already works) and accepts coexistence (rexglue D3D12 still renders its
throwaway frame). **F-4** decides the endstate: run the CP **headless** (decode only, no rexglue
render/present) — or move the decode onto the canonical Vulkan CP (`NhlVkCommandProcessor::IssueDraw`).
Deferred to F-4 by design; F-3 does not need it resolved.

## Risks

- **Per-frame decode floor.** Even with dirty-tracking, the CP must *walk* the frame's PM4 to know
  what changed. Walk-cost (no packet build) is the floor; measure in F-3.1.
- **Dynamic geometry upload.** Skinned players' verts change every frame → must re-upload, but it's
  bounded (a handful of players); the F-6 headroom (~1 ms render) absorbs a lot.
- **Identity for diffing.** "Same draw across frames" needs a stable key (draw slot + content hash).
  Slot order can shift; hash-on-change is the fallback. A wrong key = stale or thrashing — verify.
- **Live-only correctness gaps.** Residency/dynamic-texture issues that never showed in static
  replay may appear (or *resolve*, since several were takeover-capture-residency artifacts).
- **Coexistence overhead** until F-4 (double GPU + CP work). The spike says plume has the headroom;
  the CPU rebuild is the thing F-3.2/F-3.3 must tame.

## Verification harness

- `NHL_HIGHCUT_PERF` (render cost, added in the spike) + `[highcut-perf] consumer rebuild` (rebuild
  cost) — both already live in `plume_present.cpp::RenderClear` / `LoadC5Frames`.
- Single-process live recipe above; drive into a real game (controller) — live gameplay can't be
  reached autonomously, so F-3.1/F-3.5 need a human at the pad (unlike the auto-latched capture).
- Build: `scripts/_build_highcut_ffx.bat` (the surviving canonical dir).
