<#
.SYNOPSIS
    Build the project and exercise the camera.

.DESCRIPTION
    Three modes, in increasing order of how much they touch:

      list     enumerate the cameras and stop
      probe    measure the capture rate for a few seconds, writing nothing (default)
      record   run a short real recording, then verify the files it produced

    The recording goes to out/test_recordings/, never to a data drive, so a test can
    never leave files among experimental sessions.

.EXAMPLE
    .\scripts\test_camera.ps1
.EXAMPLE
    .\scripts\test_camera.ps1 -Mode record -Seconds 20 -Fps 120
#>
[CmdletBinding()]
param(
    [ValidateSet('list', 'probe', 'record')]
    [string]$Mode = 'probe',

    [string]$Serial = '26043809',
    [double]$Fps = 60,
    [int]$Frames = 600,
    [int]$Seconds = 10,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$exeDir = Join-Path $repo 'out\build\ninja-release'

function Write-Heading($text) {
    Write-Host ''
    Write-Host "=== $text ===" -ForegroundColor Cyan
}

# --- build ------------------------------------------------------------------
if (-not $SkipBuild) {
    Write-Heading 'Building'
    $env:BEHAVIOUR_CAMERA_NOPAUSE = '1'
    & cmd /c (Join-Path $repo 'build.cmd')
    if ($LASTEXITCODE -ne 0) {
        Write-Host 'Build failed; not running anything.' -ForegroundColor Red
        exit 1
    }
}

$probeExe = Join-Path $exeDir 'camera_probe.exe'
$recordExe = Join-Path $exeDir 'behaviour_camera.exe'
foreach ($exe in @($probeExe, $recordExe)) {
    if (-not (Test-Path $exe)) {
        Write-Host "Missing $exe - build first (run build.cmd)." -ForegroundColor Red
        exit 1
    }
}

# --- list -------------------------------------------------------------------
if ($Mode -eq 'list') {
    Write-Heading 'Cameras detected'
    & $probeExe
    exit $LASTEXITCODE
}

# --- probe ------------------------------------------------------------------
Write-Heading "Probing camera $Serial"
& $probeExe --serial $Serial --frames $Frames --fps $Fps
$probeExit = $LASTEXITCODE

if ($probeExit -eq 2) {
    Write-Host 'Probe could not talk to the camera; stopping here.' -ForegroundColor Red
    exit 2
}
if ($probeExit -eq 1) {
    Write-Host ''
    Write-Host 'Probe lost frames with nothing being written to disk, which points at' -ForegroundColor Yellow
    Write-Host 'the camera, cable or USB link rather than the recorder.' -ForegroundColor Yellow
}

if ($Mode -eq 'probe') {
    exit $probeExit
}

# --- record -----------------------------------------------------------------
$stamp = Get-Date -Format 'yyMMdd_HHmmss'
$sessionDir = Join-Path $repo "out\test_recordings\${stamp}_test"
New-Item -ItemType Directory -Force -Path $sessionDir | Out-Null

Write-Heading "Recording $Seconds s to $sessionDir"

# The recorder stops on Esc, or when it finds a stop-signal file named after the
# rig. The rig name comes from the table in main.cpp, falling back to cam_<serial>
# for a camera that is not listed there. This mirrors that rule so the test can
# stop the recording cleanly rather than killing it, which would skip the metadata
# write and make the verification below meaningless. Both copies disappear once the
# rig names live in a config file.
$knownRigs = @{
    '22181614' = '1'; '20530175' = '2'; '24174008' = '3'; '24243513' = '4'
    '24174020' = 'openfield'; '23606054' = 'colour_camera'; '21423798' = '6MP3_camera'
}
$rig = if ($knownRigs.ContainsKey($Serial)) { $knownRigs[$Serial] } else { "cam_$Serial" }
$stopSignal = Join-Path $sessionDir "stop_camera_$rig.signal"

$proc = Start-Process -FilePath $recordExe -PassThru -NoNewWindow -ArgumentList @(
    '--serial_number', $Serial
    '--id', 'testrun'
    '--date', $stamp
    '--fps', $Fps
    '--path', $sessionDir
)

# Touching Handle caches it, which is what makes ExitCode readable later; without
# this PowerShell reports an empty exit code after WaitForExit.
$null = $proc.Handle

Write-Host "Recording (pid $($proc.Id)). A preview window should appear."
Write-Host "Stopping automatically in $Seconds s - or press Esc in the window."

# Give the recorder a moment to fail loudly (bad serial, camera already open)
# before starting the clock on an otherwise pointless wait.
Start-Sleep -Seconds 2
if ($proc.HasExited) {
    Write-Host "Recorder exited immediately with code $($proc.ExitCode)." -ForegroundColor Red
    exit 2
}

Start-Sleep -Seconds $Seconds

if (-not $proc.HasExited) {
    Write-Host 'Writing the stop signal...'
    New-Item -ItemType File -Path $stopSignal -Force | Out-Null
    if (-not $proc.WaitForExit(30000)) {
        Write-Host 'Recorder did not stop within 30 s; terminating.' -ForegroundColor Yellow
        Write-Host 'Note: the files it wrote may be incomplete.' -ForegroundColor Yellow
        $proc.Kill()
        $proc.WaitForExit(5000) | Out-Null
    }
}
Write-Host "Recorder exited with code $($proc.ExitCode)."

# --- verify -----------------------------------------------------------------
Write-Heading 'Verifying the recording'
Write-Host ''
Get-ChildItem $sessionDir | Select-Object Name, @{n = 'MB'; e = { [math]::Round($_.Length / 1MB, 1) } } |
    Format-Table -AutoSize | Out-String | Write-Host

$checker = Join-Path $PSScriptRoot 'check_recording.py'
$python = Get-Command python -ErrorAction SilentlyContinue
if ($python) {
    & $python.Source $checker $sessionDir
    $checkExit = $LASTEXITCODE
} else {
    Write-Host 'python not on PATH, skipping the consistency check.' -ForegroundColor Yellow
    Write-Host "Run it yourself with: python scripts\check_recording.py `"$sessionDir`""
    $checkExit = 0
}

Write-Host ''
Write-Host "Test session kept at: $sessionDir"
exit $checkExit
