[CmdletBinding()]
param(
    [ValidateSet("mic", "probe")]
    [string]$Variant = "mic",
    [ValidateRange(0, 7)]
    [int]$Stage = 5,
    [ValidateSet(16000, 24000, 44100, 48000)]
    [int]$SampleRate = 24000,
    [switch]$Stage5_32Bit,
    [switch]$Rebuild
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $repoRoot "build"
$cacheFile = Join-Path $buildDir "CMakeCache.txt"
$sdkRoot = Join-Path $env:USERPROFILE ".pico-sdk\sdk\2.1.0"

$uf2Map = @{
    mic   = "examples/usb_microphone_array_6ch/usb_mic_array_6ch_pico2w.uf2"
    probe = "examples/usb_probe/usb_probe.uf2"
}

$targetMap = @{
    mic   = "usb_mic_array_6ch_pico2w"
    probe = "usb_probe"
}

$uf2Path = Join-Path $buildDir $uf2Map[$Variant]
$target = $targetMap[$Variant]
$stageArg = "-DUSB_AUDIO_DEBUG_STAGE=$Stage"
$sampleRateArg = "-DUSB_AUDIO_SAMPLE_RATE_OVERRIDE=$SampleRate"
$stage5_32BitValue = if ($Stage5_32Bit) { 1 } else { 0 }
$stage5_32BitArg = "-DUSB_AUDIO_STAGE5_32BIT=$stage5_32BitValue"
$needsBuild = $Rebuild -or -not (Test-Path $uf2Path)

function Get-BootselDrive {
    return Get-CimInstance Win32_LogicalDisk |
        Where-Object {
            $_.DriveType -eq 2 -and (
                $_.VolumeName -in @("RPI-RP2", "RP2350") -or
                ((Test-Path (Join-Path $_.DeviceID "INFO_UF2")) -and (Test-Path (Join-Path $_.DeviceID "INDEX")))
            )
        } |
        Select-Object -First 1
}

if ($needsBuild) {
    if (-not (Test-Path $sdkRoot)) {
        throw "Required SDK path not found: '$sdkRoot'. Install Pico SDK 2.1.0 first."
    }

    # Force this script to use SDK 2.1.0 even if the parent shell still exports
    # PICO_SDK_PATH for 2.0.0.
    $env:PICO_SDK_PATH = $sdkRoot

    # Always configure from a clean build tree to avoid stale SDK/toolchain cache
    # poisoning (common when switching between SDK 2.0.x and 2.1.x).
    if (Test-Path $buildDir) {
        Write-Host "Resetting build directory..." -ForegroundColor Yellow
        Remove-Item $buildDir -Recurse -Force
    }

    Write-Host "Configuring project..." -ForegroundColor Cyan
    if ($Variant -eq "mic") {
        cmake -S $repoRoot -B $buildDir -G Ninja $stageArg $sampleRateArg $stage5_32BitArg
    } else {
        # Probe target does not consume the USB audio sample-rate override.
        cmake -S $repoRoot -B $buildDir -G Ninja $stageArg
    }
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed."
    }

    if ($Variant -eq "mic") {
        $bitMode = if ($Stage5_32Bit -and $Stage -eq 5) { "32-bit" } else { "default-bitdepth" }
        Write-Host "Building firmware ($Variant, stage $Stage, ${SampleRate}Hz, $bitMode)..." -ForegroundColor Cyan
    } else {
        Write-Host "Building firmware ($Variant, stage $Stage)..." -ForegroundColor Cyan
    }
    cmake --build $buildDir --target $target
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed."
    }
} else {
    Write-Host "Using existing UF2 (skipping rebuild). Pass -Rebuild to force a fresh build." -ForegroundColor Yellow
}

if (-not (Test-Path $uf2Path)) {
    throw "UF2 not found at '$uf2Path'."
}

Write-Host "Looking for Pico in BOOTSEL mode..." -ForegroundColor Cyan
$bootDrive = Get-BootselDrive

if (-not $bootDrive) {
    # Try automatic reboot-to-BOOTSEL via picotool first.
    $picotoolCmd = Get-Command picotool -ErrorAction SilentlyContinue
    if ($picotoolCmd) {
        Write-Host "No BOOTSEL drive found; trying 'picotool reboot -f -u'..." -ForegroundColor Yellow
        & $picotoolCmd.Source reboot -f -u

        $timeoutSec = 12
        $pollIntervalMs = 500
        $deadline = (Get-Date).AddSeconds($timeoutSec)
        do {
            Start-Sleep -Milliseconds $pollIntervalMs
            $bootDrive = Get-BootselDrive
        } until ($bootDrive -or (Get-Date) -ge $deadline)
    }
}

if (-not $bootDrive) {
    throw "No Pico UF2 boot drive found. Press BOOTSEL while plugging in Pico 2 W, or ensure 'picotool reboot -f -u' works on this machine."
}

$destination = Join-Path $bootDrive.DeviceID "\" 
Write-Host "Flashing to $destination" -ForegroundColor Cyan
Copy-Item $uf2Path $destination -Force

Write-Host "Flash complete." -ForegroundColor Green
