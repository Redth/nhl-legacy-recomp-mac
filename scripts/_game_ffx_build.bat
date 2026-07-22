@echo off
call "%~dp0_env.bat" || exit /b 1
REM -fuse-ld=lld is REQUIRED now that the SDK is built with -flto=thin: its static
REM libs (e.g. snappyrd.lib) are LLVM bitcode, which the default MSVC link.exe can't
REM consume but lld can. (The shipping vk-pgo/vk-opt builds already pass it.)
cmake -S "%REPO_ROOT%" -B "out\build\win-amd64-vk-ffx" ^
  -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ ^
  -DCMAKE_EXE_LINKER_FLAGS="-fuse-ld=lld" ^
  "-DCMAKE_PREFIX_PATH=%REXGLUE_SDK_INSTALL%" ^
  -DNHLLEGACY_VULKAN_BACKEND=ON
if errorlevel 1 exit /b 1
cmake --build "out\build\win-amd64-vk-ffx" --target nhllegacy
