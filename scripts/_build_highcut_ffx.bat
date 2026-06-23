@echo off
REM Build the nhllegacy target in the surviving canonical dir (win-amd64-vk-ffx).
REM The old _build_beta.bat pointed at out/build/win-amd64-relwithdebinfo, deleted in
REM the 2026-06-17 consolidation. The plume/highcut sources are still in NHLLEGACY_SOURCES.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cmake --build "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-vk-ffx" --target nhllegacy
echo BUILD_EXIT=%ERRORLEVEL%
