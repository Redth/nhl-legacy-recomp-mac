@echo off
REM Incremental build of ONLY the Release rexruntime.dll in the canonical
REM (no-mtune) SDK build dir, for Phase-2 A/B. Output:
REM %REXGLUE_SDK_SRC%\out\win-amd64\Release\rexruntime.dll. No install.
call "%~dp0_env.bat" || exit /b 1
cmake --build "%REXGLUE_SDK_BUILD%" --config Release --target rexruntime
