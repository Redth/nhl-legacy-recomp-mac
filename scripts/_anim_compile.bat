@echo off
call "%~dp0_env.bat" || exit /b 1
cmake -S "%REPO_ROOT%" -B "out\build\win-amd64-vk-ffx" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_PREFIX_PATH=%REXGLUE_SDK_INSTALL%" -DNHLLEGACY_VULKAN_BACKEND=ON >out\build\_anim_cfg.log 2>&1
if errorlevel 1 (echo CONFIG_FAIL & exit /b 1)
ninja -C "out\build\win-amd64-vk-ffx" "CMakeFiles/nhllegacy.dir/gpu/hooks/anim_capture.cpp.obj" >out\build\_anim_obj.log 2>&1
if errorlevel 1 (echo COMPILE_FAIL & exit /b 1)
echo COMPILE_OK
