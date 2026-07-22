@echo off
call "%~dp0_env.bat" || exit /b 1
set "BLD=%REXGLUE_SDK_BUILD%"
:: PERF: SHIP the Release config (-O3 -DNDEBUG, profiling stubbed) -> rexruntime.dll
:: (no "rd" suffix); package.ps1 maps the vk-pgo/vk-opt presets to Flavor="".
:: ALSO build/install RelWithDebInfo so the dev build (win-amd64-vk-ffx, which is
:: RelWithDebInfo and links snappyrd.lib) keeps working off the same install. Both
:: configs share REXGLUE_ENABLE_TRACY=OFF from the configure step.
cmake --build "%BLD%" --config Release
if errorlevel 1 exit /b 1
cmake --install "%BLD%" --config Release
if errorlevel 1 exit /b 1
cmake --build "%BLD%" --config RelWithDebInfo
if errorlevel 1 exit /b 1
cmake --install "%BLD%" --config RelWithDebInfo
