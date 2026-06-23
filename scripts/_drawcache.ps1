# F-5 draw-level cache measurement: enables NHL_HIGHCUT_DRAWCACHE on the live EDRAM-free plume path,
# alongside the F-4 busyprobe + profile, so one dense run shows: fps gain, draw-cache hit rate, and the
# new CPU-busy split. Most of a ~1500-draw scene is STATIC (rink/boards/UI) -> high hit rate -> the
# per-draw producer work (translate/gap1/untile/packet) is skipped for those draws.
#
# YOU must drive into a game and HOLD in dense gameplay ~30s+ (autonomous attract is light). Then close.
#
# READ THE RESULT (grepped below):
#   "[highcut-perf] draw cache: H hits, M misses (NN% hit), ..."   <- want a high hit %
#   "[highcut-perf] live takeover: NN fps"                          <- compare vs ~15 fps baseline
#   "[highcut-perf] F4-busyprobe: ... our RenderBetaOwnedDraw=NNms"  <- should DROP from ~34ms
#   "[highcut-perf] window cost: ..."                                <- translate/untile/packet should drop
# A/B: re-run scripts\_f4busyprobe.ps1 (no drawcache) for the baseline, or set
#   $env:NHL_HIGHCUT_DRAWCACHE_HASHLEN="0" here to hash FULL vtx/idx content (zero added staleness).
# Staleness check: watch players/puck animate normally (the bone-palette texture's 512B prefix is in the
# signature, same boundary as the existing untile cache). If players freeze, report it.
param([string]$HashLen = "", [switch]$DrawCache)
$dir = "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-vk-ffx"
Get-Process nhllegacy -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 400
$env:NHL_VK_BACKEND_OFF        = "1"
$env:NHL_BACKEND               = "beta"
$env:NHL_BETA_TAKEOVER         = "1"
$env:NHL_BETA_LIVE             = "1"
$env:NHL_BETA_FLAT             = "1"
$env:NHL_BETA_DEPTH            = "1"
$env:NHL_BETA_LIVE_START_FRAME = "200"
$env:NHL_HIGHCUT_PRESENT       = "1"
$env:NHL_HIGHCUT_C5            = "1"
$env:NHL_HIGHCUT_LIVE_FEED     = "1"
$env:NHL_HIGHCUT_FRAME_CAPTURE = "1"
$env:NHL_HIGHCUT_PROFILE       = "1"
$env:NHL_HIGHCUT_PERF          = "1"
$env:NHL_HIGHCUT_BUSYPROBE     = "1"
$env:NHL_HIGHCUT_GEOM_BYID     = "1"   # <-- F-5 by-id geometry streaming (the camera-independent lever)
# Whole-packet DRAWCACHE is a menu-only win (camera defeats it); leave OFF by default so the geometry
# lever is measured cleanly. Pass -DrawCache to also enable it.
if ($DrawCache) { $env:NHL_HIGHCUT_DRAWCACHE = "1" } else { Remove-Item env:NHL_HIGHCUT_DRAWCACHE -ErrorAction SilentlyContinue }
if ($HashLen -ne "") { $env:NHL_HIGHCUT_DRAWCACHE_HASHLEN = $HashLen } else { Remove-Item env:NHL_HIGHCUT_DRAWCACHE_HASHLEN -ErrorAction SilentlyContinue }
$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru
Write-Output "GAME LAUNCHED (pid $($p.Id))."
Write-Output "==> DRIVE INTO A GAME and HOLD in dense gameplay ~30s+, then CLOSE the game window."
$p.WaitForExit()
Start-Sleep -Milliseconds 600
$log = (Get-ChildItem "$dir\logs\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
Write-Output "LOG=$log"
Write-Output "=== [highcut-perf] draw cache + fps + busyprobe + window cost ==="
Select-String -Path $log -Pattern "draw cache:|live takeover:|F4-busyprobe:|window cost:" | Select-Object -Last 24 | ForEach-Object { ($_.Line -split '\] ',2)[-1] }
