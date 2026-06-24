# Exploration brief: challenge the plume performance ceiling — ground-up architectural re-review

> **For:** a thorough, skeptical exploration agent (or a fresh deep-dive session) with full read access to
> this repo, the rexglue SDK at `E:\Tools\rexglue-sdk`, and the project memory/docs.
> **Mandate:** independently VERIFY the performance findings below, then CHALLENGE the architecture that
> produced them and PROPOSE concrete ground-up redesigns that break the ceiling. **Effort is NOT a
> constraint** — this is a recompilation we can modify from the ground up; propose the right architecture
> even if it is a multi-month rewrite. Bias toward "how do we make this fast," not "why it's hard."

---

## 0. Why this exploration exists (the owner's challenge — take it seriously)

A long optimization session concluded that the "plume" owned-renderer path has a **~25–33 fps dense-gameplay
ceiling** (~74 fps on light scenes), bottlenecked by "diffuse per-draw extract-and-rebuild overhead," and
that matching the retired SDK-native "FSI" backend's **66–84 fps** is "very unlikely." The project owner is
rightly skeptical: **a >10-year-old console game, on modern PC hardware, in a codebase we can modify from the
ground up, with EDRAM emulation REMOVED, should not be slower than the EDRAM-emulating path.** Those findings
are accurate *for the architecture as currently built* — but the architecture itself may be the problem.
Your job is to determine whether the ceiling is real or self-inflicted.

## 1. THE PARADOX TO RESOLVE (this is the crux — start here)

- **FSI path** (the SDK's native Vulkan backend, `NHL_HIGHCUT`-off canonical baseline) **emulates EDRAM** and
  renders dense gameplay at **66–84 fps**. See memory `sdk-vulkan-rov-backend-option`, repo
  `docs/current-status.md`, `docs/rexglue-vulkan-nhl-legacy.patch`, `docs/vulkan-migration-plan.md`.
- **Plume path** (our owned renderer, `NHL_HIGHCUT_*`/`NHL_BETA_*`) **removes EDRAM/fold/FSI** and renders
  the same scene at **~24 fps** (with the full MT producer; ~16 fps single-threaded).
- **Removing EDRAM made it ~3× slower.** Resolve this. The leading hypothesis (verify or refute):
  plume is slow **not because of EDRAM** but because it inserts a whole **decode → per-draw extract →
  snapshot → cross-thread bridge → rebuild → re-render** pipeline that the FSI backend never pays — FSI
  decodes the guest PM4 command stream and renders **inline, natively, on the same device**. If true, the
  EDRAM win is real but buried under self-imposed extraction overhead, and the fix is architectural, not
  micro-optimization.

**Key implication to test:** if the FSI native backend already runs at 66–84 fps AND the project's
`tiling-verdict-no-game-tiling` memo proves the game does **no predicated tiling** (so the EDRAM "fold" is a
pure emulation artifact, not a game requirement), then the *highest-ceiling* path may be to **strip EDRAM/fold
out of the fast native backend** rather than to build a separate slow extraction-based renderer. Evaluate this
directly.

## 2. VERIFY THESE MEASURED FINDINGS (re-confirm independently; flag anything wrong)

All measured live on an RTX 4080S-class machine, dense gameplay ~1500–1700 draws/frame, via
`scripts/_mtproducer.ps1` (gated by `NHL_HIGHCUT_MT_PRODUCER`, `NHL_HIGHCUT_F4PROBE`). The per-frame CP-thread
decomposition (steady state, dense):

| component | ms/frame | meaning |
|---|---|---|
| base-`IssueSwap` (SDK submit + swapchain present) | ~0.6 | F-4 "headless" lever — **dead** |
| ring-walk PM4 decode (between our `IssueDraw` calls) | ~9 | the SDK CP front-end |
| SDK `PrimitiveProcessor::Process()` | ~0.2 | index/prim metadata — **cheap** (strips don't convert) |
| our translate (shader xlat cache-hit + binding copies) | ~3 | |
| our snapshot (register ranges + guest vtx/idx copy) | ~2.2 | the MT threading tax |
| **"rest" (interpolator mask + per-draw SDK submission mgmt + task-fill + enqueue + bp-wait)** | **~18** | **diffuse; no fat target found** |
| worker thread (geometry gather/hash + untile + serialize) | ~30 (parallel) | |

Conclusions reached (CHALLENGE each):
- **C1.** CP-thread-bound; both CP and worker ~85–90% busy at dense; frame ≈ 40 ms ≈ 24 fps.
- **C2.** Every "fat target" hypothesis was refuted by measurement: F-4/base-swap (0.6 ms), `Process()`
  (0.2 ms), snapshot (2.2 ms), translate (3 ms). The dominant cost is **diffuse** ~18 ms "rest" at
  ~14–20 µs/draw plus the worker's ~30 ms extraction.
- **C3.** N workers help only marginally (CP-bound); custom PM4 decoder saves ≤ ~9 ms (the ring walk),
  judged not worth it because the decode is already small.
- **C4.** FSI-parity "very unlikely" because the extract→bridge→rebuild overhead is architectural.

**Verification tasks:**
1. Re-derive the CP-thread cost split from the code paths (don't just trust the probe). Confirm the probe
   instrumentation in `renderer/core/nhl_command_processor.cpp` (`NHL_HIGHCUT_F4PROBE`, `g_f4*` accumulators,
   the worker/CP busy-probes) is sound and not double-counting or missing GPU-wait vs CPU-busy.
2. **Pin the diffuse ~18 ms "rest."** This is the single biggest unexplained cost and was never broken down.
   Read `IssueDraw` (≈`nhl_command_processor.cpp:6800+`) and `RenderBetaOwnedDraw` (≈`:2136+`) and attribute
   the per-draw work that is NOT translate/snapshot/Process: the interpolator-mask computation
   (`GetInterpolatorInputMask`/`writes_interpolators`), **`BetaEnsureSubmissionOpen` / per-draw SDK
   submission management**, `CorrelateTexturesForInjection`, the register save/restore dance, the task-fill
   (binding-vector `assign`s, `result` copy), and the worker `acquire`/`enqueue` locking. Identify whether
   there IS a hidden fat target here (e.g., the SDK submission/residency bookkeeping running per draw) or it
   is genuinely diffuse. Add targeted timers if needed.
3. **Measure the FSI path's own decomposition** for the same scene (it was never profiled the same way):
   how much of its 12–15 ms frame is PM4 decode vs native render vs present? If FSI pays the same ~9 ms
   decode and renders in ~3–5 ms, that nails the paradox: our extra ~30 ms is purely the extraction layer.

## 3. ARCHITECTURAL QUESTIONS — challenge the assumptions that built the slow path

1. **Why does plume need the extract → bridge → rebuild layer at all?** Today: the CP thread decodes PM4,
   `RenderBetaOwnedDraw` extracts per-draw state into "packets," a worker rebuilds them, and a SEPARATE
   Vulkan device/thread (`gpu/hooks/plume_present.cpp`) renders. Memory `highcut-h2-plume-present` says plume
   was forced onto a *second* Vulkan device/thread because "a 2nd D3D12 device TDRs rexglue." **Re-examine
   that constraint.** Could plume render **inline** — driven directly from the PM4-decode callback on the
   SAME device the SDK already uses — exactly as the FSI backend does? That would delete the ~30 ms CP
   extract + ~30 ms worker + the cross-thread bridge and the per-draw packet rebuild, leaving decode (~9 ms)
   + a native render (~free on a 4080S). This is the highest-leverage question.
2. **Re-litigate the "inlined-PM4 ⇒ no higher hook point ⇒ D3D9-hook/static-recomp is a trap" conclusion**
   (memory `plume-exclusive-pivot`, `highcut-phase0-d3d9-hookable`, `tiling-verdict-no-game-tiling`,
   `high-cut-pivot-decision`; docs `phase0-d3d9-hookability.md`, `highcut-h1-resource-graph.md`). The team
   found NHL Legacy's per-draw `DrawIndexedPrimitive` is *inlined PM4* and concluded a high-cut/native-API
   hook is unhookable. But memory `highcut-phase0-d3d9-hookable` simultaneously says the title's D3D9 is
   "out-of-line/hookable (181 entry points, COM-style, clean present chain)." **These are in tension —
   resolve it.** Is the draw-submission inlined but the STATE-setting (SetTexture/SetRenderState/SetVertex
   Declaration/SetTransform-equivalents) still out-of-line and hookable? If most GPU *state* flows through
   hookable D3D9 entry points and only the final draw is inlined, a hybrid (hook D3D9 state + tap only the
   inlined draw) could avoid most PM4 decode AND the extraction layer.
3. **Can EDRAM/fold be removed inside the FAST native backend** instead of via a separate renderer? The
   `tiling-verdict-no-game-tiling` memo says the wide-RT fold is an EDRAM-emulation artifact that "vanishes
   for free" at the high cut. The FSI backend already renders correct geometry at 66–84 fps. What exactly in
   the SDK's EDRAM/RT-cache modeling do we object to (for the enhancement ceiling), and can we patch the SDK
   backend to drop it (resolution scale, flat RTs, no fold) while keeping its native render speed? Compare
   the effort/payoff of "patch the fast backend" vs "keep optimizing the slow extraction renderer." Memory
   `renderer-injection-seam` notes the concrete D3D12 backend IS subclassable (symbols exported).
4. **Is the per-draw re-work inherent or an artifact?** We re-translate shaders (cache-hit, but still per
   draw), re-untile textures (cache-hit), re-extract geometry, and rebuild packets every draw. The FSI/SDK
   backend persists native GPU resources (pipelines, descriptor heaps, uploaded textures, vertex/index
   buffers in shared memory) and re-uses them across frames with minimal per-draw CPU. Why doesn't plume?
   Is the by-id streaming (memory `highcut-c5g-jersey-number-residency`, the `s_sent*` caches) a poor
   substitute for just letting a native backend own residency? Quantify what a "persist native resources,
   touch only what changed" design would cost per draw.
5. **Is rexglue (a Xenia-derived PM4-level GPU emulator) the right foundation at all?** The whole premise is
   "decode the guest GPU ring buffer." A true static recompilation (XenonRecomp / "Unleashed Recompiled"
   style — see memory `unleashed-recompiled-no-edram`) translates guest CPU→native and intercepts GPU at a
   high level with NO PM4/EDRAM model. The team rejected it as a "trap" for this title due to inlined draws.
   **Stress-test that rejection.** Even if a pure D3D9-swap is infeasible, is a *hybrid* recompilation viable
   (native CPU + a thin native GPU layer fed by the few hookable state entry points + a minimal draw tap)?
   What would it take, and what is the realistic ceiling (likely native, GPU-bound, hundreds of fps)?
6. **The CPU-bound nature itself.** All findings say single-CP-thread-CPU-bound on a machine with idle cores
   and a massively idle GPU (plume render ~1–8 ms; the F-6 spike hit 165–650 fps). For a 2012 game this is
   absurd on its face. The bottleneck is entirely *our* CPU-side reconstruction. Any architecture that lets
   the GPU do the work the GPU should do (i.e., render close to how the guest intended, once) should be
   GPU-bound at hundreds of fps. Use that as the sanity north star.

## 4. DELIVERABLES (the report)

1. **Verification** — for each finding C1–C4 and the table in §2: confirmed / refuted / refined, with
   evidence (code paths + any re-measurement). Explicitly resolve the diffuse-"rest" mystery (§2.2) and the
   FSI decomposition (§2.3).
2. **Root cause of the FSI-vs-plume paradox** (§1) — a definitive explanation of why removing EDRAM made it
   3× slower, naming the exact overhead and where it lives in the code.
3. **Ranked redesign proposals** — each with: the architecture, the estimated perf ceiling (be quantitative,
   tie to the measured decode/render costs), whether it preserves the no-EDRAM/fold/FSI **enhancement
   ownership** (the whole reason plume exists — don't propose something that loses it), the effort tier
   (S/M/L/XL), the principal risks, and what would have to be true for it to work. Candidates to evaluate at
   minimum: (a) inline plume render driven from the decode (kill the bridge); (b) patch the SDK native
   backend to drop EDRAM/fold and add the enhancement seams; (c) hybrid D3D9-state-hook + draw-tap;
   (d) full/hybrid static recompilation; (e) persist-native-resources rework of the current path.
4. **A recommended path** to the **highest** sustainable framerate (target: GPU-bound, ≥ FSI's 66–84, ideally
   hundreds) that still gives us a renderer we own and can extend (shaders, resolution, post-FX, no EDRAM
   fold). Include a milestone plan and the first decisive experiment for that path.

## 5. Ground rules

- Be **skeptical of the prior conclusions, including the "FSI-parity is unlikely" verdict** — they were drawn
  while optimizing within one architecture; your job is to question the architecture.
- Read the actual SDK source (`E:\Tools\rexglue-sdk\src\src\graphics\...`: `command_processor.cpp`,
  `primitive_processor.cpp`, the Vulkan backend, the EDRAM/render-target cache) — not just our wrappers.
- Read the memory index at `C:\Users\puckh\.claude\projects\e--Repositories-nhl-legacy-recomp\memory\MEMORY.md`
  and the linked notes (esp. `plume-exclusive-pivot`, `sdk-vulkan-rov-backend-option`,
  `tiling-verdict-no-game-tiling`, `highcut-*`, `renderer-injection-seam`, `unleashed-recompiled-no-edram`)
  and the docs they reference. Treat memory as "true when written" — re-verify against current code.
- Quantify. Tie every proposal's ceiling to measured numbers (decode ~9 ms, native render ~1–8 ms, the
  extraction ~60 ms split across threads). The math should make the recommended path's ceiling obvious.
- Effort is explicitly NOT a gating factor. A correct XL rewrite that hits 100+ fps beats an S micro-opt that
  hits 30.
