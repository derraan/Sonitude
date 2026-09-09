param(
    [string]$Workspace = "C:\Users\darre\Sound_Bubble",
    [string]$Bridge    = "C:\Users\darre\Documents\Obsidian Notes\CDE3301\colab_bridge",
    [switch]$PullRuns  # Opt-in: runs/ is large. Zoo is always pulled.
)

$ErrorActionPreference = "Stop"

$BridgeZoo  = Join-Path $Bridge "zoo"
$BridgeRuns = Join-Path $Bridge "runs"
$WsZoo      = Join-Path $Workspace "edge\zoo"
$WsRuns     = Join-Path $Workspace "runs"

if (-not (Test-Path $BridgeZoo)) {
    Write-Host "[pull] bridge zoo not found at $BridgeZoo -- nothing to pull" -ForegroundColor Yellow
    exit 0
}
if (-not (Test-Path $WsZoo)) { New-Item -ItemType Directory -Path $WsZoo | Out-Null }

Write-Host "[pull] $BridgeZoo -> $WsZoo"
robocopy $BridgeZoo $WsZoo /E /NFL /NDL /NJH /NP /R:2 /W:2 | Out-Host
if ($LASTEXITCODE -ge 8) { Write-Host "zoo pull failed ($LASTEXITCODE)" -ForegroundColor Red; exit $LASTEXITCODE }

if ($PullRuns) {
    if (-not (Test-Path $WsRuns)) { New-Item -ItemType Directory -Path $WsRuns | Out-Null }
    Write-Host "[pull] $BridgeRuns -> $WsRuns"
    robocopy $BridgeRuns $WsRuns /E /NFL /NDL /NJH /NP /R:2 /W:2 | Out-Host
    if ($LASTEXITCODE -ge 8) { Write-Host "runs pull failed ($LASTEXITCODE)" -ForegroundColor Red; exit $LASTEXITCODE }
}

Write-Host "[pull] done. Zoo contents:"
Get-ChildItem $WsZoo -File | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize
