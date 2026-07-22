# fetch_ffmpeg.ps1 - download the LGPL static ffmpeg.exe that the release bundles.
#
# The VP6 host-decode bridge (src/vp6_bridge.cpp) spawns ffmpeg.exe to decode the
# game's *.vp6 movies (the recompiled guest VP6 IDCT has an unfixed arithmetic bug;
# see docs/vp6-fork-investigation.md). release/package.ps1 stages this binary beside
# nhllegacy.exe so movies play out of the box.
#
# We ship the LGPLv3 static build (BtbN, `--enable-version3` without `--enable-gpl`)
# to match release/THIRD-PARTY-NOTICES.txt. It is run as a separate process (mere
# aggregation) - no linking against the game. The binary is NOT committed to git
# (~109 MB); this script fetches it into release/vendor/ffmpeg/ (gitignored).
#
# Usage:  powershell -ExecutionPolicy Bypass -File release\fetch_ffmpeg.ps1
param(
    [string]$Url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-lgpl.zip"
)
$ErrorActionPreference = "Stop"
$vendor = Join-Path $PSScriptRoot "vendor\ffmpeg"
New-Item -ItemType Directory -Force $vendor | Out-Null
$exe = Join-Path $vendor "ffmpeg.exe"
if (Test-Path $exe) {
    Write-Host "ffmpeg.exe already present at $exe - delete it to re-fetch."
    exit 0
}

$tmp = Join-Path $env:TEMP "ffmpeg-lgpl-fetch"
if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
New-Item -ItemType Directory -Force $tmp | Out-Null
$zip = Join-Path $tmp "ffmpeg-lgpl.zip"

Write-Host "[ffmpeg] downloading $Url"
Invoke-WebRequest -Uri $Url -OutFile $zip
Write-Host "[ffmpeg] extracting"
Expand-Archive -Path $zip -DestinationPath $tmp -Force

$srcExe = Get-ChildItem -Path $tmp -Recurse -Filter "ffmpeg.exe" | Select-Object -First 1
$srcLic = Get-ChildItem -Path $tmp -Recurse -Filter "LICENSE.txt" | Select-Object -First 1
if (-not $srcExe) { throw "ffmpeg.exe not found in the downloaded archive" }
Copy-Item $srcExe.FullName $exe -Force
if ($srcLic) { Copy-Item $srcLic.FullName (Join-Path $vendor "ffmpeg-LICENSE.txt") -Force }
Remove-Item -Recurse -Force $tmp

# Sanity: must be an LGPL build with the VP6 decoder.
$cfg = & $exe -hide_banner -version 2>&1 | Out-String
if ($cfg -match "--enable-gpl") {
    Write-Warning "This is a GPL build - THIRD-PARTY-NOTICES lists ffmpeg as LGPL. Prefer the *-lgpl* artifact."
}
$dec = & $exe -hide_banner -decoders 2>&1 | Out-String
if ($dec -notmatch "\bvp6\b") { Write-Warning "ffmpeg has no vp6 decoder - VP6 movies will not decode." }
Write-Host "[ffmpeg] ready -> $exe"
