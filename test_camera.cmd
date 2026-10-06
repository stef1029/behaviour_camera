@echo off
REM Build and test the camera. Double-click this file, or run it from a terminal.
REM
REM   test_camera.cmd            build, then measure the capture rate (writes nothing)
REM   test_camera.cmd list       just list the cameras it can see
REM   test_camera.cmd record     also record a short session and verify the files
REM
REM Anything after the mode is passed through to scripts\test_camera.ps1, so:
REM   test_camera.cmd record -Seconds 30 -Fps 120
REM   test_camera.cmd probe -Serial 22181614

setlocal
cd /d "%~dp0"

set "MODE=%~1"
if "%MODE%"=="" set "MODE=probe"
shift

REM Collect any remaining arguments to forward to the PowerShell script.
set "EXTRA="
:collect
if "%~1"=="" goto run
set "EXTRA=%EXTRA% %1"
shift
goto collect

:run
powershell.exe -NoProfile -ExecutionPolicy Bypass ^
    -File "%~dp0scripts\test_camera.ps1" -Mode %MODE%%EXTRA%
set "RESULT=%ERRORLEVEL%"

echo.
if "%RESULT%"=="0" echo TEST PASSED.
if "%RESULT%"=="1" echo TEST RAN, BUT FRAMES WERE DROPPED - see the output above.
if "%RESULT%"=="2" echo TEST FAILED - see the output above.

if not defined BEHAVIOUR_CAMERA_NOPAUSE pause
exit /b %RESULT%
