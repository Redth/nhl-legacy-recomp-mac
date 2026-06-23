# MT producer — Stage 1 execution plan (single worker, prove the seam)

> Decided 2026-06-23 after F-5b proved the snapshot is ~30× cheaper than the work (MT is GO).
> Staging chosen by the user: **1 worker first** (prove the extraction live), then widen to the
> N-worker pool. Gated behind `NHL_HIGHCUT_MT_PRODUCER` (default OFF → byte-identical serial path).
> Target: ~1.8× (~29 fps) by overlapping our ~34 ms producer work with the SDK's ~25 ms PM4 decode.
> Code: `renderer/core/nhl_command_processor.cpp` (`RenderBetaOwnedDraw`, ~:1405).

## The seam (verified by full read of the function)

`RenderBetaOwnedDraw` is one ~1800-line method. The clean CP-thread / worker split:

| Lines | Role | Thread |
|---|---|---|
| 1405–2144 | setup, **translate** (SDK `SpirvShaderTranslator` — NOT thread-safe), owned-render skip, `vpi` compute | **CP thread** (unchanged) |
| 2149–2920 + 3073–3190 | **producer body**: geometry gather+hash → float pack → `spv_sys` → untile → sampler descs → `hdr` → stream resources → serialize → push | **WORKER** |
| 2922–3068 | **frame-commit**: present-boundary detect, `HighcutLiveCommitFrame`, counter reset, fps/perf report | **CP thread** (moves to a drain) |

**Why this split:** translate calls into non-thread-safe SDK code and is 100% cache-hit in steady
state (cheap), so it stays serial. The body is pure compute on register values + guest bytes +
`s_texCache` (the only shared mutable state it touches) + stable `Shader*`/`memory_`. The frame-commit
is per-frame and order-sensitive.

## What must be snapshotted (the worker can't read live state)

The SDK rewrites `register_file_` and **ring-buffers guest vtx/idx within a frame** (draw N's vertex
region is overwritten by a later draw in the SAME frame), so a worker on draw N would read corrupt
data. Snapshot per draw into a heap `HcDrawTask`:

1. **`RegisterFile rf`** — full copy (`*register_file_`; it's a flat POD `uint32_t values[0x5003]`,
   ~82 KB, trivially copyable). Stage 1 copies the whole thing (~4 ms/frame, on the CP thread,
   overlapped) to avoid register-index bookkeeping; **Stage 2 trims to the two ranges SNAPPROBE used**
   (`[0x2000,0x2400)` + `[0x4000,0x4940)`) to cut it to ~1 ms.
2. **Guest vertex bytes** — per bound stream (`beta_current_vs_->vertex_bindings()`, fetch const →
   `base = addr<<2`, `size = sz<<2`). Snapshot the bytes; the gather (2333/2346) reads the snapshot,
   NOT `memory_->TranslatePhysical`.
3. **Guest index bytes** — `index_buffer_info->{guest_base,length,format}` + the bytes (idx gather 2366).
4. **`vpi`** (`draw_util::ViewportInfo`), **`result`** (`PrimitiveProcessor::ProcessingResult`, POD),
   **`primitive_type`**, **`index_count`**, **`ndc`/`ncm`/`bound_rt_bits`/`rt_formats`** if the body
   reads them (it reads `vpi`, `result`, `primitive_type`, `index_count`; `ndc` only in the skipped
   owned-render path — verify).
5. **Translate outputs** (computed CP-side at 1692–1965): `p3_vs_spirv`, `p3_vs_id`, `p3_vs_texbinds`,
   `p3_vs_sampbinds`, `p3_vs_sampler_count`, `p3_ps_spirv`, `p3_ps_id`, `p3_ps_texbinds`,
   `p3_ps_sampbinds`, `p3_ps_sampler_count`, `p3_dump_data`.
6. **Stable pointers** read directly via `this` on the worker (NO copy — set once / read-only during a
   frame): `memory_` (texture source only — textures are NOT ring-buffered, stable within a frame given
   the per-frame drain barrier), `beta_current_vs_`, `beta_current_ps_`/`eff_ps`, `beta_rt_width_`,
   `beta_rt_height_`. Pass `beta_current_vs_`/`eff_ps` in the task (they're per-draw).

## Body redirects (mechanical, ~24 sites)

- `const uint32_t* regs = register_file_->values;` (2151) → `= t.rf.values;`
- every `register_file_->Get<reg::X>()` / `(*register_file_)[i]` in the body → `t.rf.Get<reg::X>()` /
  `t.rf[i]`. (~18 sites: RB_DEPTHCONTROL, RB_DEPTH_INFO, RB_COLOR_INFO, RB_COLOR_MASK, RB_SURFACE_INFO,
  PA_SU_SC_MODE_CNTL, PA_CL_VTE_CNTL, PA_SC_WINDOW_SCISSOR_TL/BR, VGT_INDX_OFFSET, VGT_MIN/MAX_VTX_INDX,
  kBlendControl[0], etc.)
- geometry gather `memory_->TranslatePhysical(base)` (2333, 2346) and idx (2366) → read from the task's
  snapshot buffers (keep `memory_` for **texture** source only at 2615/2691).
- `vpi`/`result`/`primitive_type`/`index_count`/`p3_*` → `t.*`.

## Worker + lifecycle (Stage 1 = single worker)

- `HcLiveWorker`: one `std::thread`, a `std::deque<std::unique_ptr<HcDrawTask>>` guarded by a
  `std::mutex` + `std::condition_variable`, and a `drain()` that blocks until the queue is empty AND the
  worker is idle. The worker pops in FIFO order and calls `this->ProduceLiveDrawPacket(*t)` — which runs
  the body and does the in-order `HighcutLivePushResource`/`HighcutLivePushDraw` (single worker ⇒ push
  order == draw order, NO reorder buffer, NO `s_texCache` lock needed — the worker is the only thread
  touching `s_texCache`/`s_sentRes`/`s_sentGeo`/`highcut_capture_idx_`).
- **CP thread** in `RenderBetaOwnedDraw`: detect the present boundary at the top (compare
  `HighcutGuestPresentCount()` to a stored value). On change: `worker.drain()` → run the frame-commit
  (`CommitLiveFrame()`: `HighcutLiveCommitFrame` + counter reset + fps/perf report) → continue. Then,
  per draw: setup+translate+vpi, build the snapshot task, `worker.enqueue(std::move(task))`, return.
- Process shutdown / takeover end: `worker.drain()` + join.

## Increments (each compiles; land in order)

- **1a — extract, still synchronous (behavior-identical, compile + reason verified).** Move the body
  into `ProduceLiveDrawPacket(const HcDrawTask&)`; the non-MT path builds a stack task from LIVE state
  (rf snapshot NOT needed when synchronous — but build it anyway so 1b is a flag flip) and calls it
  inline. Frame-commit stays inline for now. **No perf change; pure refactor — the safety net.**
- **1b — single worker + drain.** Route MT → enqueue+return; move frame-commit to `CommitLiveFrame()`
  called at the CP-thread drain. This is where the overlap (and the win) appears.
- **Validate:** `scripts/_mtproducer.ps1` (clone `_snapprobe.ps1`, add `NHL_HIGHCUT_MT_PRODUCER=1`).
  User drives dense gameplay; read `live takeover: N fps` (expect ~16→~29) and confirm geometry renders
  correctly (A/B by unsetting the flag — must be byte-identical to the proven serial path).

## Then Stage 2 (the full ~38 fps): widen to N workers

Add an `s_texCache` mutex + a seq-ordered drain (write each packet to `slot[seq]`, drain in order),
keep streaming/push at the ordered drain. Translate stays CP-side. Trim the rf snapshot to ranges.
Target ~38 fps; 60 still needs F-4 (throwaway base-render cut) and/or the leaner custom PM4 decoder.
