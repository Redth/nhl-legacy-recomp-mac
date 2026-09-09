# macOS: where gameplay frame time actually goes

Measured 2026-09-09 on M4 / MoltenVK, scripted gameplay route, ~48 ms frames
(~19-20 fps) at ~1820 draws/frame.

| path | cost per frame | share |
|------|----------------|-------|
| `IssueDraw` (1.85–2.34 us/draw x ~1820) | ~3.4 ms | ~7% |
| `IssueSwap` (EndSubmission + readback flush + present) | 0.45–0.73 ms | ~1.3% |
| `IssueCopy` (~7 resolves x 4.6–8.9 us) | ~0.04 ms | ~0.1% |
| **total graphics command submission** | **~4 ms** | **~8%** |
| unaccounted (recompiled guest CPU) | ~44 ms | ~92% |

## This contradicts docs/cpu-perf-optimization-plan.md

That plan states the game is "draw-submission / single-thread CPU bound
(~1600 draws/frame, ~10–18 us/draw)". That was measured on the **Windows**
build. On macOS/arm64 the same path costs **1.85–2.34 us/draw**, five to eight
times less, and the whole graphics submission layer is ~8% of frame time.

**Consequence: the six-item plan cannot deliver meaningful macOS fps.** Even
eliminating 100% of graphics submission would move 48 ms to 44 ms - 19 fps to
about 20.7. Items #1 and #2 are implemented (see below) and are individually
sound, but their ceiling here is a fraction of a percent. Items #3 (sampler
caching) and #6 (multithreaded command recording) target the same ~8% and should
NOT be prioritised on macOS on the strength of that plan.

## What was implemented anyway, and what it is worth

Both are correct, both remove real work, both are kept - they are simply not
where macOS frame time is.

- **#1 descriptor set reuse** (3d4a110). 51.7% of texture descriptor-set
  allocations and `vkUpdateDescriptorSets` eliminated (94.0% vertex, 29.3%
  pixel). Timed: the descriptor path went 255 -> 199 ns/draw, about **0.16 ms on
  a 52 ms frame**.
- **#2 pipeline state description short-circuit** (68138c9). 69.0% of draws skip
  `GetCurrentStateDescription`, against a ~70% ceiling.

## Where macOS fps actually has to come from

The ~92% is the recompiled guest code - the title's own simulation and command
generation - not the graphics layer. Relevant existing levers are PGO (plan
Phase 5) and `-mtune` (plan item #4, currently unset), both of which act on the
recompiled binary rather than the SDK.

Menus sit at exactly 30.0 fps / 33.3 ms, a hard cap. Gameplay falls to ~19-20,
so the guest simply cannot produce frames faster.

## How to measure (do not use fps)

Live runs diverge and never render the same content, so frame-to-frame or
run-to-run comparison is meaningless - measured 11 vs 30 samples over one
nominal frame range. The deterministic replay bench the plan prescribes is
unavailable on macOS: SDK 0.10.x removed `rex/graphics/trace_protocol.h`, so
`tools/replay/src/xtr_player_unsupported.cpp` is linked instead of the real
player, and the SDK ships no trace writer at all (the `trace_gpu_*` cvars are
inert shims in `src/sdk_cvar_compat.cpp`).

Use instead: `NHL_DRAW_TIMING=1` (per-draw, per-resolve and per-swap wall time)
and `REX_CPU_TIMING=1` (nanoseconds in the descriptor path). Per-draw
nanoseconds inside a named code path is content-normalised and comparable across
runs. `REX_DESCRIPTOR_STATS=1`, `REX_PIPELINE_STATS=1` and `REX_SHORTCUT_STATS=1`
report hit rates.
