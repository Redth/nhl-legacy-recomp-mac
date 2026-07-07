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
