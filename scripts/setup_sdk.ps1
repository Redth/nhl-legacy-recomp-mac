# Fetch + pin + patch the ReXGlue SDK this port builds against, and fetch the
# vendored third-party deps (plume / XenosRecomp). Idempotent - re-run to repair.
#
# The SDK is public (https://github.com/rexglue/rexglue-sdk, BSD-3). This project
# pins the exact commit the port was developed against and applies the title-
# specific patch (sdk/rexglue-vulkan-nhl-legacy-bd9b519.patch): Vulkan texture/
# translator fixes (signed BC5 normals, cube sanitizer, exp_adjust), the
# NHL_VK_READBACK_MODE perf override, and the prebuilt-FidelityFX import path.
#
# Usage:  pwsh scripts/setup_sdk.ps1   (or powershell -File scripts/setup_sdk.ps1)
#   -SdkDir <path>   where to put the SDK source (default third_party/rexglue-sdk)
param(
    [string]$SdkDir = "",
    [string]$Pin = "bd9b51918faa71f962fa38af6d5f744af9aba2ce"
)
$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
if (-not $SdkDir) { $SdkDir = Join-Path $repo "third_party\rexglue-sdk" }
$patch = Join-Path $repo "sdk\rexglue-vulkan-nhl-legacy-bd9b519.patch"

# 1. Clone / pin the SDK.
if (-not (Test-Path (Join-Path $SdkDir ".git"))) {
    Write-Host "[sdk] cloning rexglue-sdk -> $SdkDir"
    git clone --recursive https://github.com/rexglue/rexglue-sdk $SdkDir
    if ($LASTEXITCODE -ne 0) { throw "git clone failed" }
}
Write-Host "[sdk] pinning $Pin"
git -C $SdkDir checkout $Pin
if ($LASTEXITCODE -ne 0) { throw "git checkout $Pin failed" }
git -C $SdkDir submodule update --init --recursive
if ($LASTEXITCODE -ne 0) { throw "git submodule update failed" }

# 2. Apply the title patch (skip if already applied).
git -C $SdkDir apply --check $patch 2>$null
if ($LASTEXITCODE -eq 0) {
    git -C $SdkDir apply $patch
    if ($LASTEXITCODE -ne 0) { throw "patch apply failed" }
    Write-Host "[sdk] title patch applied"
} else {
    git -C $SdkDir apply --check --reverse $patch 2>$null
    if ($LASTEXITCODE -eq 0) {
        Write-Host "[sdk] title patch already applied - OK"
    } else {
        throw "[sdk] patch does not apply (tree modified?). Reset with: git -C `"$SdkDir`" checkout -- . ; then re-run."
    }
}

# 3. Vendored high-cut deps (required unconditionally by CMakeLists for now).
& (Join-Path $repo "tools\fetch_thirdparty.ps1")

Write-Host ""
Write-Host "SDK ready at $SdkDir (commit $Pin + title patch)."
Write-Host "Next: scripts\_ffx_sdk_configure.bat  (see BOOTSTRAP.md for the full order)"
