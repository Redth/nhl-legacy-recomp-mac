# PARALLEL-PRODUCER SNAPSHOT-COST PROBE — the DECISIVE measurement for off-thread parallelization
# (handoff f5). Does the per-draw producer work (~34ms) parallelize off the CP thread, or does the
# input SNAPSHOT it requires eat the win?
#
# To move the work to a worker pool, the CP thread must first SNAPSHOT each draw's inputs from MUTABLE
# SDK state (register banks + guest vertex/index bytes; the SDK rewrites them between draws). That copy
# is the IRREDUCIBLE new CP-thread cost, so the post-MT CP wall ~= SDK-decode + snapshot. This probe
# (NHL_HIGHCUT_SNAPPROBE) copies exactly those bytes into a scratch buffer, times them on the CP thread,
# and reports ms/frame — WITHOUT building the pool. It changes nothing about rendering.
#
# YOU must drive into a game and HOLD in dense gameplay ~30s+ (autonomous attract never reaches it),
# then close the game. This prints the [highcut-perf] SNAPPROBE lines.
#
# READ THE RESULT — "SNAPPROBE: per-draw input snapshot = NN ms/frame (MM MB/frame, K draws/frame)":
#   snapshot << ~34ms  (e.g. <~8ms)  => the byte copy is cheap => the MT producer is GO: a worker pool
#                                       can take the CP wall to ~SDK-decode (~25ms) + snapshot => ~30ms
#                                       => ~33fps, then F-4 trims further.
#   snapshot ~= ~34ms                 => the raw byte copy alone is as expensive as the work => threads
#                                       do NOT help; 60fps needs the leaner custom PM4 decoder instead.
#                                       (If so, the NEXT probe models a DEDUPED snapshot — copy only the
#                                        register banks + by-id ids, not full vtx/idx — a cheaper floor.)
# Context lines (busyprobe + fps + window cost) are printed too so the snapshot reads against our 34ms.
$dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-vk-ffx"
Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
# Force the D3D12 backend (beta takeover needs it; the build auto-forces VK otherwise).
$env:NHL_VK_BACKEND_OFF        = "1"
$env:NHL_BACKEND               = "beta"
$env:NHL_BETA_TAKEOVER         = "1"
$env:NHL_BETA_LIVE             = "1"
$env:NHL_BETA_FLAT             = "1"
$env:NHL_BETA_DEPTH            = "1"
$env:NHL_BETA_LIVE_START_FRAME = "200"
$env:NHL_HIGHCUT_PRESENT       = "1"
$env:NHL_HIGHCUT_C5            = "1"
$env:NHL_HIGHCUT_LIVE_FEED     = "1"   # BOTH live_feed + frame_capture required (gate at :1686/:2733)
$env:NHL_HIGHCUT_FRAME_CAPTURE = "1"
$env:NHL_HIGHCUT_PROFILE       = "1"   # producer in-function bucket split (our ~34ms reference)
$env:NHL_HIGHCUT_PERF          = "1"   # plume render cost
$env:NHL_HIGHCUT_BUSYPROBE     = "1"   # CPU-busy vs blocked context
$env:NHL_HIGHCUT_SNAPPROBE     = "1"   # <-- THE decisive snapshot-cost probe
$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru
Write-Output "GAME LAUNCHED (pid $($p.Id))."
Write-Output "==> DRIVE INTO A GAME and HOLD in dense gameplay ~30s+, then CLOSE the game window."
$p.WaitForExit()
Start-Sleep -Milliseconds 600
$log = (Get-ChildItem "$dir\logs\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
Write-Output "LOG=$log"
Write-Output "=== [highcut-perf] SNAPPROBE (snapshot cost vs our ~34ms) ==="
Select-String -Path $log -Pattern "SNAPPROBE" | Select-Object -Last 12 | ForEach-Object { ($_.Line -split '\] ',2)[-1] }
Write-Output "=== [highcut-perf] context: busyprobe + fps + window cost ==="
Select-String -Path $log -Pattern "F4-busyprobe|live takeover:|window cost:" | Select-Object -Last 16 | ForEach-Object { ($_.Line -split '\] ',2)[-1] }
