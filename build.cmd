@echo off
REM Build the project. Double-click, or run from a terminal.
REM
REM   build.cmd              build Release (RelWithDebInfo: optimised, with symbols)
REM   build.cmd debug        build Debug
REM   build.cmd vs           generate a Visual Studio 2026 solution instead
REM   build.cmd vs2022       generate a Visual Studio 2022 solution instead
REM   build.cmd clean        delete the build tree and build Release from scratch

setlocal
cd /d "%~dp0"

call "%~dp0scripts\dev_env.cmd" || goto :failed

set "PRESET=ninja-release"
if /i "%~1"=="debug"  set "PRESET=ninja-debug"
if /i "%~1"=="vs"     set "PRESET=vs2026"
if /i "%~1"=="vs2026" set "PRESET=vs2026"
if /i "%~1"=="vs2022" set "PRESET=vs2022"

if /i "%~1"=="clean" (
    echo Removing out\build\ninja-release ...
    if exist "out\build\ninja-release" rmdir /s /q "out\build\ninja-release"
)

echo.
echo === Configuring (%PRESET%) ===
cmake --preset %PRESET% || goto :failed

echo.
echo === Building (%PRESET%) ===
echo %PRESET% | findstr /b /c:"vs" >nul
if not errorlevel 1 (
    cmake --build --preset %PRESET%-release || goto :failed
    echo.
    echo Solution: out\build\%PRESET%\behaviour_camera.sln
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
