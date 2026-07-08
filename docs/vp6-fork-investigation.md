# VP6 green-corruption investigation — fork session log (2026-07-06)

Symptom (unchanged from upstream project): boot movies (`ealogo.vp6` etc.) play
with 8x8-block-aligned green/red corruption; DC-only/flat regions decode fine,
high-AC blocks are garbage. Prior diagnosis (src/diag_hooks.cpp:149): a
recompiled arithmetic-precision bug in the VP6 dequant/inverse transform.

## Ruled out today

1. **Upstream `bcctr` tail-recovery codegen fix (10cf1ad)** — cherry-picked as
   `sdk/rexglue-codegen-bcctr-tail-fix.patch` (now part of the pinned patch
   set). It DOES change this title's codegen (8 of 185 generated files,
   `nhllegacy_recomp.104-111.cpp`) and is kept for correctness — but the movie
   corruption is unchanged (before/after screenshots:
   `out/fork_boot_smoke.png` first frame vs `out/vp6_after_fix.png`).
2. **Sibling saturating-op codegen bugs.** The pin commit fixed `vaddsws`
   (`blendv_epi8` selects per-byte but the overflow mask is per-word). Audited
   `src/codegen/builders/vector.cpp`: `vaddsbs/vaddshs/vsubsbs/vsubshs` use
   native SSE saturating intrinsics (fine), `vsubsws` is scalar 64-bit clamped
   (fine). No remaining blendv-pattern bugs in the add/sub family.
3. **Unimplemented `vmhaddshs`/`vmhraddshs`** (AltiVec IDCT workhorses): absent
   from the codegen builders, but ALSO absent from the generated title code —
   the game's VP6 IDCT is not built on them (likely scalar integer math).
4. **The singleton vtable lead** (old probe comment): `*(0x83B3AA10)` →
   obj `0x83B5D9D4`, vtable `0x823A404C`. `vtable[0] = sub_833FBEA0` is an
   **empty function (single blr)** — a no-op virtual, not the transform.
   Entries 2..11 (`sub_82764EE8..sub_82765760`, in
   `generated/default/nhllegacy_recomp.26.cpp`) are thin state wrappers.

## Confirmed call chain (probe run, this fork build)

`src/diag_hooks.cpp` `g_vp6_probe` + `LogGuestStackHere` in the
`sub_8276AC70` hook (flip the flag + rebuild to reproduce):

```
thread entry 83074164 -> 8307D72C -> 8307D668
  -> 82671744            (movie decode job / caller of the frame driver)
  -> 8277AE48 / 8277AF68 (TWO call sites inside the frame driver fn ~0x8277A___
                          - loops macroblocks)
  -> sub_8276AC70        (per-block driver; r3=ctx, r4/r5/r6 block ptrs)
```

First-call args observed: `r3=BFB37A68 r4=BD3E0B60 r5=BD3E0B50 r6=004AB608`,
then steady-state `r3=BFB37AC8 r4=BD95F37C r5=8232E02A r6=707BFBC0`.

## Next steps (in preference order)

1. **Differential block harness** (the dev's original plan): in the
   `sub_8276AC70` hook, dump the input coefficient block and the output pixels
   (before/after calling the real impl) for a few hundred blocks; decode the
   same movie with FFmpeg (`ffmpeg -i ealogo.vp6`) and implement the reference
   VP6 dequant+IDCT on host; diff to find WHICH arithmetic step diverges; then
   inspect that step's generated C++ for the miscompiled construct.
   Buffer layout of r4/r5/r6 must be established first (dump hex around them).
2. **Host-decode bridge** (bypasses the bug entirely, fixes ALL movies): hook
   the frame driver (fn containing 0x8277AE48) — replace guest video decode
   with FFmpeg VP6 (ffmpeg decodes VP6 fine; see tools/vp6-converter README),
   write Y/Cb/Cr planes into the guest output buffers. Need: frame driver's
   arg layout (input bitstream ptr + output plane ptrs). The shipped 0.2.0
   rexruntime.dll dumps `vp6_luma.raw` (1280x720 Y) + `vp6_chroma.raw` per run
   — the dev's SDK tree had a plane-buffer tap (never captured in the patch),
   proving the planes are identifiable SDK-side too.
3. Watch upstream: any future `fix(codegen)` arithmetic commits — re-test by
   cherry-picking onto the pin (the `CHANGED_FILES` diff check in this
   session's workflow tells you immediately whether a codegen fix affects this
   title: rebuild SDK, `mv generated generated_prev`, re-run codegen, diff).

## Tooling notes

- Boot+screenshot driver: `out/vp6_smoke.ps1` (50s wait catches the movie on
  this machine; 95s reaches the title screen).
- The probe prints `[diag] vp6_blockdrv stack: ...` lines into
  `logs/nhllegacy_*.log` and `vp6_probe.txt` next to the exe.

## Session 2 additions (2026-07-07)

- Reference decode works: `ffmpeg -i ealogo.vp6 frames_%03d.png` (365 frames;
  ignore the EA-audio "revision2" warning). Dark frames decode clean in-game;
  bright high-AC content corrupts — consistent with the arithmetic diagnosis.
- Job-record layout at the block driver (recon harness, `vp6_harness.txt`):
  records of `{count, dataPtr, completionFn(=sub_826FF200/826FF220 - event
  signalers), srcPtr, dstPtr}`. The block driver `sub_8276AC70` posts command
  **#26** via `vtable[0]` of the manager singleton at `*(0x83B3AA10)`
  (obj 0x83B5D9D4, vt 0x823A404C) — but that vt[0] (sub_833FBEA0) is an empty
  `blr`, so consumption is asynchronous (worker ring), NOT through this call.
- The two immediate-constant IDCTs (`sub_827D2FE8` file 30, `sub_82898660`
  file 37 — hooks left in diag_hooks) do NOT fire during the movie. The VP6
  transform is elsewhere (likely table-driven constants), unfound so far.
- Frame driver = `sub_8277ABB8` (file 27, lines ~35420); direct callees swept
  (`vp6_sweep.txt`): per-frame setup only. Codec context object = `0xFE0B7280`.
- NEXT: dump the codec object at frame-driver entry; find plane pointers
  (candidates look like 0x707xxxxx physical); diff planes vs the ffmpeg
  reference per frame. Then either characterize the arithmetic bug (Path A)
  or inject host-decoded planes per frame at frame-driver level (Path B).
- **Frame-driver signature established** (frame tap, `vp6_frame.txt`):
  `sub_8277ABB8(r3=codec_obj@0xFE0B6BC0, r4=video_chunk_desc, r5=subtitle_desc,
  r6=0x000C0020, r7=r4)`. The r4 descriptor contains the EA container tags
  ("MV0F", "SCDl") + a chunk offset table + the raw chunk data pointer
  (0xBD8A4910 on frame 0) — **the compressed VP6 frame input is fully located
  (Path B input side solved).** r5 carries the .sub subtitle stream (UTF-16
  text visible). Codec object's first 768B = job-queue nodes/list heads; NO
  plane pointers there.
- NEXT capture: hook the frame driver's CALLER (the movie-job fn containing
  return addr 0x82671744) and dump its state AFTER the frame-driver returns —
  the decoded plane pointers should be fetched there (getFrame pattern) before
  texture upload. Once planes are located: diff vs ffmpeg reference frame
  (Path A characterization) or overwrite with host-decoded planes (Path B fix).
- Post-decode callees sub_82670768/sub_82671568 = generic utilities (event
  handles 0xF80000xx, UI string tables) — not the frame publish path.
- **BEST NEXT LEAD (do this first next session):** the movie renders via YUV
  textures — boot log line "VulkanTextureCache: Format k_Cr_Y1_Cb_Y0_REP ...
  fallback". Add a small tap in the SDK's vulkan/texture_cache.cpp upload path
  for k_Cr_Y1_Cb_Y0_REP / k_Y1_Cr_Y0_Cb_REP textures: log the guest source
  address + dims per upload during the movie. That is exactly where the
  original dev's shipped-runtime vp6_luma.raw/vp6_chroma.raw tap lived (16
  "vp6_luma" string hits in shipped rexruntime.dll, never captured in the
  patch). Once plane addresses are known: dump planes per frame → diff vs
  docs' ffmpeg reference (Path A characterization), or overwrite the planes /
  swap the texture upload source with host-decoded frames (Path B fix at the
  cleanest possible seam — host-side, no guest-memory writes needed).

## Session 3 (2026-07-07) — BREAKTHROUGH: the decoder is innocent

Method: SDK-side upload tap (NHL_VP6_TAP in vulkan/texture_cache.cpp
LoadTextureDataFromResidentMemoryImpl) + game-side RGBA sampler thread
(NHL_VP6_RGBA=1CF32000 in diag_hooks) + Xenos untile in python
(GetTiledOffset2D port).

Facts established:
1. The movie is presented via a per-frame re-upload of ONE texture:
   k_8_8_8_8 (fmt=6) 1280x720 at guest phys 0x1CF32000 (stable across runs;
   393 uploads/movie). The guest decodes VP6 on CPU, the GPU converts YUV->RGB
   into a render target, and the RESOLVE of that RT lands at 0x1CF32000 in
   Xenos TILED layout.
2. The resolve needs CPU readback for the guest copy: our size gate
   (NHL_VK_READBACK_MAX_LEN=3MB) skips this 3.68MB surface, so guest RAM reads
   zero; capture runs need NHL_VK_READBACK_MODE=full +
   NHL_VK_READBACK_MAX_LEN=999999999.
3. **The decoded+converted frames are PIXEL-PERFECT.** Untiled dumps
   (out/build/.../vp6_untiled_*.rgba, PNGs in the session notes) show crisp
   legal text + clouds, zero corruption. The long-standing "recompiled
   arithmetic-precision bug in VP6 dequant/IDCT" theory is DEAD.
4. Therefore the corruption enters between the resolve and the screen — the
   texture cache / shared memory consumption of the resolve (GPU-resident
   path). Channel/endian swap of the final buffer does NOT reproduce the
   on-screen pattern (smooth tint, not blocks); the blocky dot-crawl says
   tiling-interpretation or stale/racing data.
5. With FULL ungated readback, the on-screen movie corruption drops to sparse
   small green bars (out/vp6_fullrb_movie.png) vs full-screen blocks with the
   gate. Consistent with a resolve->texture invalidation/race: correct CPU
   data mostly wins, GPU-resident stale reads still leak through.
6. This is the SAME bug class that forced readback_resolve=full for equipment
   compositing. One real fix in the SDK's resolve-consumption path likely
   fixes movies AND equipment, and could obsolete full readback (perf win).

Next steps:
1. Extend the tap to log texture_key.tiled / scaled_resolve / endian +
   whether the load sourced shared memory vs the scaled-resolve buffer for
   the movie texture.
2. Inspect VulkanTextureCache/SharedMemory: how a range written by resolve
   (MarkRangeAsResolved) is consumed by a texture load of the same range —
   look for tiling mismatch or missing invalidation when the texture was
   cached before the resolve.
3. Candidate cheap mitigation while the real fix lands: exempt the movie
   resolve (or fmt=6 1280x720 resolves) from the size gate + force texture
   invalidation after readback.

## Session 4 (2026-07-07 afternoon) — ROOT CAUSE: lost invalidations in the
## frame-end page-state clear (SetSystemPageBlocksValidWithGpuDataWritten)

Post-session-3 fixes already in the SDK tree when this session started (all
kept, all defensible, none cured the movie):
- `VulkanSharedMemory::GetUsageMasks` kComputeWrite declared READ instead of
  WRITE access (real barrier bug; after the fix a full sync-validation run —
  `out/vp6_syncval.ps1`, VK_LAYER settings validate_sync=true — reports ZERO
  hazards through boot+movie+menu).
- `vp6_repack` in texture_cache.cpp (buffer-copy bypass of the load shader
  for row-padded non-tiled k_8) — its vkCmdCopyBuffer was ILLEGAL until this
  session added VK_BUFFER_USAGE_TRANSFER_DST_BIT to the scratch buffer
  (VUID-vkCmdCopyBuffer-dstBuffer-00120, 20 hits in the syncval log).

Instrumentation added (all env-gated, kept in tree):
- `NHL_VP6_SM=<hexbase>:<hexlen>` — graphics/shared_memory.cpp logs every
  gpu-written / cpu-invalidate / ram-upload touching the range;
  vulkan/shared_memory.cpp re-compares each upload snapshot against guest RAM
  (torn-upload detector).
- `NHL_VP6_REQ` — logs every RequestRanges merged range >= 128KB.
- The NHL_VP6_TAP / REQ caps were raised 400 -> 20000: **the 400-line caps
  filled during BOOT (fmt=6 loads run at ~50/s from +2s), so all session-3
  per-movie upload counts were actually pre-movie noise.** Movie window in a
  50s smoke run is roughly +40s..+52s.

Facts established by the traces (gated build, corruption reproducing):
1. Movie texture 0x1CF32000 (fmt=6 tiled): 1588 gpu-written markings, ONE
   initial ram-upload, ZERO cpu-invalidate over a whole run. Its bytes come
   purely from the GPU resolve; nothing overwrites them; sync is clean.
   => corruption is UPSTREAM of the resolve (the session-3 "resolve->texture
   consumption" theory is dead).
2. The YUV source region (fmt=18 texture at 0x1C98E000 + surroundings) shows
   per-movie-frame cpu-invalidate (256KB-widened) + partial ram-uploads
   (~350-500KB/frame — the EA player only rewrites CHANGED macroblock
   regions). Torn-upload detector: 0 hits (uploads are stable during memcpy).
3. Solid green = ZERO YUV bytes (Y=Cb=Cr=0 -> G~135). The corrupted areas
   follow high-activity content; dark/static content is clean. So the GPU
   consumed pages whose CPU updates never made it into the shared-memory
   buffer.
4. out/vp6_fullrb_movie.png ("full readback nearly fixes it") is a DARK
   frame — the always-clean content class. That comparison was confounded;
   readback mode was never the mechanism.

ROOT CAUSE (src/graphics/shared_memory.cpp): this SDK refactored Xenia's
`SetSystemPageBlocksValidWithGpuDataWritten` (valid := valid_and_gpu_written,
i.e. drop CPU-page validity each frame; runs EVERY frame-end because
`clear_memory_page_state` defaults TRUE here, unlike upstream) into a
double-buffered pointer swap (`active_valid_flags_`/`staging_valid_flags_`)
executed WITHOUT the global critical region. Guest threads clearing validity
in `MemoryInvalidationCallback` (lock held) race the swap: their clears land
in the swapped-out buffer and are LOST -> pages stay VALID while guest RAM
has newer data -> RequestRanges skips the upload -> the GPU reads stale/zero
bytes. For the movie that drops decoded macroblock updates (green blocks,
worst when decode is heavy near frame end = bright/high-AC frames). Same
lost-invalidation class as the equipment-compositing corruption. The
incremental "dirty blocks" path additionally reverted non-dirty blocks to a
two-swaps-old snapshot.

FIX: rewrite the function as an in-place
`memcpy(active, valid_and_gpu_written)` under the global lock, no pointer
swap (staging buffer now unused). Residual (second-order, pre-existing):
RequestRanges' lock-free all-valid fast path is check-then-act without the
lock; an invalidation landing right after the check can still be missed for
one frame. If any corruption remains, take the lock around that check next.

## Session 4 continued — REVERSAL: the decoder IS guilty (for high-AC content)

After the page-state fix, dark content (arena title screen, menu movies) is
now CLEAN with the 3 MB gate — the "sparse green bars" class is gone. But the
EA-logo movie still corrupts heavily. Further findings:

1. **The EA-logo movie's real data path** (uncapped tap; the old 400-line tap
   caps filled during BOOT — all session-3 "per-movie" counts were pre-movie
   noise): ring of FOUR non-tiled k_8 luma textures 1280x720 pitch 1280 at
   0x196B1000 / 0x19827000 / 0x1999D000 / 0x19A83000 + 640x360 pitch-768 k_8
   chroma planes (several slots: 0x1C9B6000, 0x19C65000, 0x197DF000,
   0x19797000, 0x19955000, 0x1990D000, ...), each reloaded every frame. The
   fmt=18 packed CrY1CbY0 texture at 0x1C98E000 belongs to the CLOUD movie
   (language/legal screens' background), which renders fine — session 3's
   "pixel-perfect" dumps were of THAT easy low-AC content, which is why the
   decoder looked innocent.
2. **Torn uploads are real but secondary**: the guest decoder rewrites ring
   slots while VulkanSharedMemory::UploadRanges memcpys them (85 torn uploads
   per movie, up to 722 KB of a plane changed mid-copy; heavy CP-thread load
   amplifies it dramatically — the per-byte diagnostic itself made the movie
   far worse). FIXED with a bounded re-copy-until-stable loop in UploadRanges
   (memcmp+yield, 8 tries). With the fix, all uploads read "stable" — and the
   striping is UNCHANGED, so tears were not the visible corruption.
3. **The decoded luma plane IN GUEST RAM is corrupted** (game-side dumper
   NHL_VP6_RGBA=196B1000 → vp6_rgba_*.raw; one 3.6 MB dump covers 3 of the 4
   luma slots; out/vp6_luma_slot0.png): dense black vertical dashes over the
   bright, high-detail EA-ball content. GPU/emulator side fully exonerated
   for this class — this is decoder output.
4. **Arithmetic signature** (intrinsic analysis of the dump): dash pixels are
   luma CLAMPED TO ZERO (96% in [0,16], mean 3 — underflow, not wrap), and
   within 8x8 blocks they hit specific columns: x%8 = 7 strongly (4x
   baseline), 2 and 3 moderately (~2x); y%8 is FLAT. Uniform-in-y +
   column-specific = the ROW (horizontal) IDCT pass emits large negative
   values at specific output columns for every row when high-AC coefficients
   are present; the final clamp crushes the whole column to black. Dark/flat
   content has no high-AC energy → decodes clean → explains every historical
   "dark frames fine, bright frames break" observation.

NEXT (decoder investigation restart, from the session-2 plan but now with the
correct targets):
- Differential harness at the block driver `sub_8276AC70` (hooks still in
  src/diag_hooks.cpp): dump coefficient input + reconstructed output for
  blocks landing in luma columns-7-heavy regions; implement the reference VP6
  row IDCT on host; binary-search which term diverges; then find the
  miscompiled PPC construct in the generated C++ (suspects: multiply-high /
  saturating pack / arithmetic-shift rounding in the row butterfly).
- OR Path B host-decode bridge: input side solved (session 2 - MV0F chunk
  descriptor), output side now solved too (planar ring addresses above for
  fullscreen movies; fmt=18 packed buffer for the cloud/menu movies).

## Automation note

The boot flow shows a language-select screen when the profile save is absent/
incomplete (it waits for gamepad input — the user's DS4 "X"; keyboard/window
message injection does NOT reach the SDL/MnK input path from an unfocused
background script). For unattended movie captures: copy the user data tree to
a scratch dir MINUS the incomplete `BEAPRO 20260707175841` + `PROFILE
20260707175842` entries (half-written by a hard-killed run; their presence
triggers first-boot flow) and launch with
`--user_data_root "...\out\testuser"`. See out/vp6_auto1.ps1.
