@echo off
call "%~dp0_env.bat" || exit /b 1
set "BLD=%REXGLUE_SDK_BUILD%"
:: Clear the poisoned cache (CMAKE_GENERATOR_PLATFORM) but keep _deps (FidelityFX clone).
if exist "%BLD%\CMakeCache.txt" del /q "%BLD%\CMakeCache.txt"
if exist "%BLD%\CMakeFiles" rmdir /s /q "%BLD%\CMakeFiles"
:: PERF: ship the runtime DLL as a -O3 + ThinLTO build with NO profiling
:: instrumentation. The release links the Release config (see
:: _ffx_sdk_build_install.bat) where REXGLUE_ENABLE_PROFILING is already stubbed,
:: but we also turn Tracy/perf-counters OFF outright so TracyClient.dll is not a
:: runtime dependency and the perf-counter registry is gone entirely. -flto=thin
:: (+ lld) does intra-DLL whole-program opt on the EDRAM/command-processor/Vulkan
:: hot paths (docs/current-status.md: dense gameplay is draw-submission/CPU bound).
:: -march=x86-64-v3 is a PORTABLE floor (AVX2/BMI/MOVBE, no SSE4a) — see
:: commit 49e6650; do NOT use -march=native (SSE4a EXTRQ #UD on Intel).
cmake -S "%REXGLUE_SDK_SRC%" -B "%BLD%" ^
  -G "Ninja Multi-Config" ^
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
  "-DCMAKE_C_FLAGS=-march=x86-64-v3 -flto=thin" "-DCMAKE_CXX_FLAGS=-march=x86-64-v3 -flto=thin" ^
  "-DCMAKE_SHARED_LINKER_FLAGS=-flto=thin -fuse-ld=lld" ^
  "-DCMAKE_EXE_LINKER_FLAGS=-flto=thin -fuse-ld=lld" ^
  -DCMAKE_CXX_STANDARD=23 ^
  "-DCMAKE_CONFIGURATION_TYPES=Debug;Release;RelWithDebInfo" ^
  "-DCMAKE_CROSS_CONFIGS=all" "-DCMAKE_DEFAULT_CONFIGS=all" -DCMAKE_DEFAULT_BUILD_TYPE=Release ^
  "-DCMAKE_INSTALL_PREFIX=%REXGLUE_SDK_INSTALL%" ^
  -DREXGLUE_USE_D3D12=ON -DREXGLUE_USE_VULKAN=ON ^
  -DREXGLUE_ENABLE_FIDELITYFX=ON -DREXGLUE_FIDELITYFX_BACKEND=vk ^
  "-DREXGLUE_FIDELITYFX_PREBUILT_DIR=%REXGLUE_FFX_PREBUILT%" ^
  -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=OFF
