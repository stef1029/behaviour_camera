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

set "VSWHERE_DIR=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer"
if not exist "%VSWHERE_DIR%\vswhere.exe" (
    echo ERROR: vswhere.exe not found. Is Visual Studio installed?
    exit /b 1
)

REM Capture vswhere's answer through a temp file rather than a for /f backtick
REM command: a quoted executable path inside backticks parses awkwardly, and set /p
REM sidesteps the question entirely.
set "VSPATH_FILE=%TEMP%\behaviour_camera_vspath.txt"
"%VSWHERE_DIR%\vswhere.exe" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%VSPATH_FILE%" 2>nul
set "VSPATH="
if exist "%VSPATH_FILE%" set /p VSPATH=<"%VSPATH_FILE%"
del "%VSPATH_FILE%" 2>nul

if not defined VSPATH (
    echo ERROR: No Visual Studio installation with the C++ toolset was found.
    echo Install the "Desktop development with C++" workload.
    exit /b 1
)

REM stderr is discarded as well as stdout. vcvars64.bat on VS 2026 prints
REM "'vswhere.exe' is not recognized as an internal or external command" to stderr
REM from inside its own detection chain, then carries on and sets the environment
REM correctly (exit code 0, VCToolsInstallDir populated). Verified by calling
REM vcvars64.bat on its own: the message is Microsoft's, not this script's. Left
REM visible it appears on every build and looks like a failure. A real failure is
REM still caught by the errorlevel check and by the compiler check below.
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed.
    exit /b 1
)

REM Confirm the environment actually came up, since vcvars' own output is hidden.
if not defined VCToolsInstallDir (
    echo ERROR: the MSVC environment was not set up by vcvars64.bat.
    exit /b 1
)

REM Ninja and a recent CMake both ship with the VS CMake component.
set "VSCMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if exist "%VSCMAKE%\Ninja\ninja.exe" set "PATH=%VSCMAKE%\Ninja;%PATH%"
if exist "%VSCMAKE%\CMake\bin\cmake.exe" set "PATH=%VSCMAKE%\CMake\bin;%PATH%"

set "BEHAVIOUR_CAMERA_ENV_READY=1"
exit /b 0
