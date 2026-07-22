@echo off
call "%~dp0_env.bat" || exit /b 1
cmake --build "e:\Repositories\nhl-legacy-recomp\out\build\win-amd64-relwithdebinfo" --target nhllegacy
echo BUILD_EXIT=%ERRORLEVEL%
