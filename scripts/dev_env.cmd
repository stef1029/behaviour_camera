@echo off
REM Sets up the MSVC build environment for this shell.
REM
REM Called by build.cmd and test_camera.cmd; not usually run directly. Locates
REM Visual Studio with vswhere rather than a hardcoded path, so this keeps working
REM across VS versions and editions, and adds the CMake extension's bundled Ninja
REM to PATH (vcvars does not, because Ninja ships with the CMake component).
REM
REM Note for anyone editing this: PATH must be extended *after* vcvars runs, which
REM is why this is a batch file rather than a single chained command. In a batch
REM file each line is expanded as it executes; in a one-liner %PATH% would expand
REM before vcvars had changed it.

if defined BEHAVIOUR_CAMERA_ENV_READY goto :eof

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo ERROR: vswhere.exe not found. Is Visual Studio installed?
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -products * ^
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSPATH=%%i"
)

if not defined VSPATH (
    echo ERROR: No Visual Studio installation with the C++ toolset was found.
    echo Install the "Desktop development with C++" workload.
    exit /b 1
)

call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed.
    exit /b 1
)

REM Ninja and a recent CMake both ship with the VS CMake component.
set "VSCMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if exist "%VSCMAKE%\Ninja\ninja.exe" set "PATH=%VSCMAKE%\Ninja;%PATH%"
if exist "%VSCMAKE%\CMake\bin\cmake.exe" set "PATH=%VSCMAKE%\CMake\bin;%PATH%"

set "BEHAVIOUR_CAMERA_ENV_READY=1"
exit /b 0
