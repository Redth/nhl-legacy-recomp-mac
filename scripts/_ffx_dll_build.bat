@echo off
call "%~dp0_env.bat" || exit /b 1
set "FFXAPI=%REXGLUE_SDK_BUILD%\_deps\fidelityfx-src\ffx-api"
cmake -S "%FFXAPI%" -B "%FFXAPI%\build_vk" -G "Visual Studio 17 2022" -A x64 ^
  -DFFX_API_BACKEND=VK_X64 -DFFX_API_AUTO_COMPILE_SHADERS=ON
if errorlevel 1 exit /b 1
cmake --build "%FFXAPI%\build_vk" --config Release --parallel
if errorlevel 1 exit /b 1

REM Stage the DLL + import lib where _ffx_sdk_configure.bat's
REM REXGLUE_FIDELITYFX_PREBUILT_DIR expects them, so the SDK reconfigure picks
REM up the prebuilt import path instead of the (clang/Ninja-incompatible)
REM in-tree FidelityFX build.
if not exist "%REXGLUE_FFX_PREBUILT%" mkdir "%REXGLUE_FFX_PREBUILT%"
set "FFX_FOUND="
for /r "%FFXAPI%\build_vk" %%f in (amd_fidelityfx_vk.dll) do (
  if exist "%%f" if not defined FFX_FOUND (
    set "FFX_FOUND=1"
    copy /y "%%f" "%REXGLUE_FFX_PREBUILT%\" >nul
    if exist "%%~dpnf.lib" copy /y "%%~dpnf.lib" "%REXGLUE_FFX_PREBUILT%\" >nul
    echo staged %%f -^> %REXGLUE_FFX_PREBUILT%
  )
)
if not defined FFX_FOUND (
  echo ERROR: amd_fidelityfx_vk.dll not found under %FFXAPI%\build_vk
  exit /b 1
)
if not exist "%REXGLUE_FFX_PREBUILT%\amd_fidelityfx_vk.lib" (
  echo WARNING: amd_fidelityfx_vk.lib not found next to the DLL - locate it under
  echo   %FFXAPI%\build_vk and copy it to %REXGLUE_FFX_PREBUILT% manually.
)
