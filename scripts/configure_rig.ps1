<#
.SYNOPSIS
    Apply the Windows settings that affect capture reliability on a rig machine.

.DESCRIPTION
    None of these are code changes, and all of them have been implicated in dropped
    frames. Run once per rig machine, as Administrator.

      * High performance power plan. The default Balanced plan parks CPU cores, ramps
        frequency lazily, and leaves USB selective suspend enabled - a well known
        cause of intermittent USB3 camera dropouts.

      * Antivirus exclusion for the capture directory. Real-time scanning of a
        sustained write stream causes latency spikes, and a spike costs frames.

    Only the settings that affect capture. A rig also doing live pose needs its
    GPU clocks locked as well, which is PoseLink's scripts/configure_gpu.ps1.

    Reports what it finds first and changes nothing unless -Apply is given, so it is
    safe to run just to see how a machine is set up.

.EXAMPLE
    .\scripts\configure_rig.ps1
.EXAMPLE
    .\scripts\configure_rig.ps1 -Apply -CapturePath D:\behaviour_data
#>
[CmdletBinding()]
param(
    [switch]$Apply,
    [string]$CapturePath = "E:\test_vid_output"
)

$ErrorActionPreference = 'Continue'

function Test-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    (New-Object Security.Principal.WindowsPrincipal $identity).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
}

if ($Apply -and -not (Test-Admin)) {
    Write-Host 'This needs to run as Administrator to change anything.' -ForegroundColor Red
    Write-Host 'Right-click PowerShell and choose "Run as administrator".'
    exit 1
}

Write-Host ''
Write-Host '=== Power plan ===' -ForegroundColor Cyan
$scheme = (powercfg /getactivescheme) -join ''
Write-Host "  current: $scheme"

# The well-known GUID for High performance, identical across Windows installs.
$highPerformance = '8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c'
if ($scheme -match $highPerformance) {
    Write-Host '  already on High performance' -ForegroundColor Green
} elseif ($Apply) {
    powercfg /setactive $highPerformance
    if ($LASTEXITCODE -eq 0) {
        Write-Host '  switched to High performance' -ForegroundColor Green
    } else {
        Write-Host '  could not switch; the plan may be hidden by group policy' -ForegroundColor Yellow
    }
} else {
    Write-Host '  WOULD switch to High performance (re-run with -Apply)' -ForegroundColor Yellow
}

Write-Host ''
Write-Host '=== USB selective suspend ===' -ForegroundColor Cyan
# Part of the active plan rather than a global switch; High performance disables it,
# but it is set explicitly here because a plan can have been edited.
if ($Apply) {
    powercfg /setacvalueindex SCHEME_CURRENT 2a737441-1930-4402-8d77-b2bebba308a3 48e6b7a6-50f5-4782-a5d4-53bb8f07e226 0
    powercfg /setactive SCHEME_CURRENT
    Write-Host '  disabled for the active plan' -ForegroundColor Green
} else {
    Write-Host '  WOULD disable it for the active plan (re-run with -Apply)' -ForegroundColor Yellow
}

Write-Host ''
Write-Host '=== Antivirus exclusion ===' -ForegroundColor Cyan
Write-Host "  capture path: $CapturePath"
try {
    $status = Get-MpComputerStatus -ErrorAction Stop
    if (-not $status.RealTimeProtectionEnabled) {
        Write-Host '  Defender real-time protection is off; no exclusion needed' -ForegroundColor Green
    } else {
        $existing = (Get-MpPreference -ErrorAction Stop).ExclusionPath
        if ($existing -contains $CapturePath) {
            Write-Host '  already excluded' -ForegroundColor Green
        } elseif ($Apply) {
            Add-MpPreference -ExclusionPath $CapturePath
            Write-Host '  exclusion added' -ForegroundColor Green
        } else {
            Write-Host '  WOULD add an exclusion (re-run with -Apply)' -ForegroundColor Yellow
        }
    }
} catch {
    # A third-party product, or Defender disabled entirely. Either way this script
    # cannot manage it, and saying so is more useful than failing.
    Write-Host '  could not query Defender - it may be disabled or replaced by' -ForegroundColor Yellow
    Write-Host '  another product. If so, exclude the capture directory by hand.' -ForegroundColor Yellow
}

Write-Host ''
Write-Host '=== Capture drive ===' -ForegroundColor Cyan
$drive = (Split-Path -Qualifier $CapturePath).TrimEnd(':')
$volume = Get-Volume -DriveLetter $drive -ErrorAction SilentlyContinue
if ($volume) {
    $freeGB = [math]::Round($volume.SizeRemaining / 1GB)
    $totalGB = [math]::Round($volume.Size / 1GB)
    $percentFree = [math]::Round(100 * $volume.SizeRemaining / $volume.Size)
    Write-Host "  ${drive}: $freeGB GB free of $totalGB GB ($percentFree%)"
    # SSDs lose a great deal of sustained write speed as they fill, and dropped
    # frames get noticeably worse with them.
    if ($percentFree -lt 20) {
        Write-Host '  WARNING: under 20% free. Sustained write speed falls sharply on a' -ForegroundColor Red
        Write-Host '  full SSD, and that shows up as dropped frames.' -ForegroundColor Red
    }
    # One camera at 1280x1024 Mono8: 141 GB/hour at 30 fps.
    Write-Host ("  at 30 fps that is about {0:N1} hours of recording for one camera" -f ($freeGB / 141))
} else {
    Write-Host "  drive $drive not found" -ForegroundColor Yellow
}

Write-Host ''
Write-Host ''
if (-not $Apply) {
    Write-Host 'Nothing was changed. Re-run as Administrator with -Apply to make these changes.' -ForegroundColor Cyan
}
