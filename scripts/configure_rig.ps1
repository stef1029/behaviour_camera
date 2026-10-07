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

      * Locked GPU clocks, for live pose estimation only. An idle GPU drops its
        graphics clock to ~210 MHz and its memory clock to ~810 MHz, and takes
        long enough to come back up that an occasional inference costs ten times
        what a continuous one does. Measured on an RTX 4000 Ada: 8 ms back to
        back, 94 ms with a one second gap between requests. Locking both clocks
        makes it a flat 10 ms at any request rate, for about 15 W at idle. A rig
        that is not doing live pose does not need this.

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
Write-Host '=== GPU clocks (live pose only) ===' -ForegroundColor Cyan
$smi = Get-Command nvidia-smi -ErrorAction SilentlyContinue
if (-not $smi) {
    Write-Host '  no nvidia-smi; skipping (not needed without live pose)'
} else {
    $q = (& nvidia-smi --query-gpu=name,clocks.sm,clocks.max.sm,clocks.mem,clocks.max.mem `
            --format=csv,noheader,nounits) -split ',' | ForEach-Object { $_.Trim() }
    if ($q.Count -ge 5) {
        $name = $q[0]
        $sm = [int]$q[1]; $smMax = [int]$q[2]
        $mem = [int]$q[3]; $memMax = [int]$q[4]
        Write-Host "  $name"
        Write-Host "  graphics $sm MHz of $smMax max, memory $mem MHz of $memMax max"

        # Idle clocks are a small fraction of maximum. Anything near maximum
        # while the machine is doing nothing means they are already locked.
        $locked = ($sm -gt ($smMax * 0.6)) -and ($mem -gt ($memMax * 0.6))
        if ($locked) {
            Write-Host '  clocks look locked up already' -ForegroundColor Green
        } elseif ($Apply) {
            # Lock to the highest clock the card actually sustains rather than
            # its absolute maximum, which it will not hold anyway.
            $smLock = [int]($smMax * 0.75)
            & nvidia-smi -lgc "$smLock,$smLock" | Out-Null
            $okSm = ($LASTEXITCODE -eq 0)
            & nvidia-smi -lmc "$memMax,$memMax" | Out-Null
            $okMem = ($LASTEXITCODE -eq 0)
            if ($okSm -and $okMem) {
                Write-Host "  locked graphics to $smLock MHz and memory to $memMax MHz" -ForegroundColor Green
                Write-Host '  NOTE: this does not survive a reboot. Add it to a startup task,'
                Write-Host '  or re-run this script, if the rig is restarted.'
            } else {
                Write-Host '  could not lock the clocks; some cards and drivers refuse' -ForegroundColor Yellow
            }
        } else {
            Write-Host '  clocks are idling. For live pose this costs up to 10x on latency;' -ForegroundColor Yellow
            Write-Host '  re-run with -Apply to lock them. Harmless to skip otherwise.' -ForegroundColor Yellow
        }
    }
}

Write-Host ''
if (-not $Apply) {
    Write-Host 'Nothing was changed. Re-run as Administrator with -Apply to make these changes.' -ForegroundColor Cyan
}
