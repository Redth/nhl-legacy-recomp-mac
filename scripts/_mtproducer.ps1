# MT PRODUCER -- Stage 1a verification (docs/mt-producer-stage1-plan.md).
#
# 1a routes the per-draw packet production through the lifted-out ProduceLiveDrawPacket method, run
# SYNCHRONOUSLY with use_snapshot=false (reads LIVE register_file_/memory_). So this is the EXTRACTION
# FAITHFULNESS test: flag-ON must render IDENTICALLY to flag-OFF (the proven serial body). If geometry/
# textures/fps match, the ~350-line transcription is faithful and 1b (snapshot + worker thread) is just
# flipping use_snapshot=true and moving the call onto a thread.
#
# USAGE:
#   scripts\_mtproducer.ps1          # flag ON  (NHL_HIGHCUT_MT_PRODUCER=1) -- the new path
#   scripts\_mtproducer.ps1 -Off     # flag OFF (proven serial path) -- the A/B reference
#
# YOU drive into a game and HOLD in dense gameplay ~30s+, then close. Compare the two runs by eye
# (players/rink/HUD render the same?) and by the fps line. The MT path logs "[highcut-mt] live takeover
# (MT): N fps"; the serial path logs "[highcut-perf] live takeover: N fps".
#
# 1b-step2 (now default): the per-draw work runs on a WORKER THREAD overlapping the SDK PM4 decode, so
# fps should RISE (~16 -> ~29 dense) AND the render must still match flag-OFF. Debug ladder (set before
# launch to bisect a problem):
#   NHL_HIGHCUT_MT_SYNC=1       run the consumer on the CP thread (no worker)  -- isolates threading
#   NHL_HIGHCUT_MT_SYNC=1 + NHL_HIGHCUT_MT_LIVEREAD=1   sync + live read (no snapshot) -- the 1a path
param([switch]$Off)
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
if ($Off) {
  Remove-Item Env:\NHL_HIGHCUT_MT_PRODUCER -ErrorAction SilentlyContinue
  Write-Output "=== MT PRODUCER OFF (proven serial path -- A/B reference) ==="
} else {
  $env:NHL_HIGHCUT_MT_PRODUCER = "1"
  Write-Output "=== MT PRODUCER ON (Stage 1a: ProduceLiveDrawPacket, synchronous, live read) ==="
}
$argline = '--game_data_root "H:\Emulators\games\XBOX\NHL Legacy - Vanilla"'
$p = Start-Process -FilePath "$dir\nhllegacy.exe" -ArgumentList $argline -WorkingDirectory $dir -PassThru
Write-Output "GAME LAUNCHED (pid $($p.Id))."
Write-Output "==> DRIVE INTO A GAME and HOLD in dense gameplay ~30s+, then CLOSE the game window."
Write-Output "==> VERIFY: flag-ON render must MATCH flag-OFF (run both, compare by eye)."
$p.WaitForExit()
Start-Sleep -Milliseconds 600
$log = (Get-ChildItem "$dir\logs\*.log" | Sort-Object LastWriteTime -Descending | Select-Object -First 1).FullName
Write-Output "LOG=$log"
Write-Output "=== [highcut-mt] (MT path notice + fps) ==="
Select-String -Path $log -Pattern "highcut-mt" | Select-Object -Last 12 | ForEach-Object { ($_.Line -split '\] ',2)[-1] }
Write-Output "=== [highcut-perf] live takeover fps (serial path, if -Off) ==="
Select-String -Path $log -Pattern "live takeover:" | Select-Object -Last 6 | ForEach-Object { ($_.Line -split '\] ',2)[-1] }
