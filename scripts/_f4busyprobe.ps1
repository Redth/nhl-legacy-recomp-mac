# F-4 first-step DECISIVE measurement: is the ~31ms OUTSIDE RenderBetaOwnedDraw
# CPU-busy (SDK PM4 decode -> F-4 still needs it) or BLOCKED (coexistence GPU wait -> F-4 frees it)?
#
# Launches the live EDRAM-free plume path (single process, full live recipe) with the new
# NHL_HIGHCUT_BUSYPROBE=1 instrumentation. YOU must drive into a game and HOLD in dense gameplay
# for ~30s+ (autonomous attract never reaches dense gameplay). Then close the game; this prints
# the [highcut-perf] F4-busyprobe lines.
#
# READ THE RESULT:
#   "CP-thread CPU-busy=NN% ... blocked(coexist GPU wait)=NNms/frame"
#     busy << wall  (e.g. busy 50%, blocked ~30ms/frame) => the CP thread IDLES on coexistence
#                   => F-4 (suppress rexglue render/present) reclaims that ~31ms ~for free (~2x fps).
#     busy ~= wall  (busy ~90-100%, blocked ~0)          => the outside time is SDK PM4 DECODE (CPU)
#                   => F-4 does NOT free it; it's needed work. Re-plan (attack our 34ms / draw cache).
#   Second line (needs NHL_HIGHCUT_PROFILE, which is on here) splits CPU-busy into
#   our RenderBetaOwnedDraw vs SDK-PM4-decode(+other).
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
$env:NHL_HIGHCUT_PROFILE       = "1"   # producer in-function bucket split (translate/untile/packet/gap)
$env:NHL_HIGHCUT_PERF          = "1"   # plume render cost
$env:NHL_HIGHCUT_BUSYPROBE     = "1"   # <-- the F-4 decision probe (CPU-busy vs blocked)
$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru
Write-Output "GAME LAUNCHED (pid $($p.Id))."
Write-Output "==> DRIVE INTO A GAME and HOLD in dense gameplay ~30s+, then CLOSE the game window."
$p.WaitForExit()
Start-Sleep -Milliseconds 600
$log = (Get-ChildItem "$dir\logs\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
Write-Output "LOG=$log"
Write-Output "=== [highcut-perf] F4-busyprobe (CPU-busy vs blocked) ==="
Select-String -Path $log -Pattern "F4-busyprobe" | Select-Object -Last 12 | ForEach-Object { ($_.Line -split '\] ',2)[-1] }
Write-Output "=== [highcut-perf] live takeover fps + window cost (context) ==="
Select-String -Path $log -Pattern "live takeover:|window cost:" | Select-Object -Last 8 | ForEach-Object { ($_.Line -split '\] ',2)[-1] }
