@echo off
REM Build the project. Double-click, or run from a terminal.
REM
REM   build.cmd              build Release (RelWithDebInfo: optimised, with symbols)
REM   build.cmd debug        build Debug
REM   build.cmd vs           generate a Visual Studio solution instead
REM   build.cmd clean        delete the build tree and build Release from scratch

setlocal
cd /d "%~dp0"

call "%~dp0scripts\dev_env.cmd" || goto :failed

set "PRESET=ninja-release"
if /i "%~1"=="debug" set "PRESET=ninja-debug"
if /i "%~1"=="vs"    set "PRESET=vs2026"

if /i "%~1"=="clean" (
    echo Removing out\build\ninja-release ...
    if exist "out\build\ninja-release" rmdir /s /q "out\build\ninja-release"
)

echo.
echo === Configuring (%PRESET%) ===
cmake --preset %PRESET% || goto :failed

echo.
echo === Building (%PRESET%) ===
if /i "%PRESET%"=="vs2026" (
    cmake --build --preset vs2026-release || goto :failed
    echo.
    echo Solution: out\build\vs2026\behaviour_camera.sln
) else (
    cmake --build --preset %PRESET% || goto :failed
    echo.
    echo Binaries: out\build\%PRESET%\
)

echo.
echo Build succeeded.
if "%~1"=="" if not defined BEHAVIOUR_CAMERA_NOPAUSE pause
exit /b 0

:failed
echo.
echo BUILD FAILED.
if not defined BEHAVIOUR_CAMERA_NOPAUSE pause
exit /b 1
