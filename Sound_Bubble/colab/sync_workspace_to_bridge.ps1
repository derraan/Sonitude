param(
    [string]$Workspace = "C:\Users\darre\Sound_Bubble",
    [string]$Bridge    = "C:\Users\darre\Documents\Obsidian Notes\CDE3301\colab_bridge"
)

$ErrorActionPreference = "Stop"

$BridgeCode = Join-Path $Bridge "code"
if (-not (Test-Path $Bridge))     { New-Item -ItemType Directory -Path $Bridge     | Out-Null }
if (-not (Test-Path $BridgeCode)) { New-Item -ItemType Directory -Path $BridgeCode | Out-Null }
foreach ($sub in @("datasets","runs","zoo")) {
    $p = Join-Path $Bridge $sub
    if (-not (Test-Path $p)) { New-Item -ItemType Directory -Path $p | Out-Null }
}

# Excluded dirs are big, local-only, or regenerable. robocopy /MIR mirrors the
# source tree so deleted files on the PC side are removed from the bridge.
$ExDirs  = @(".venv","venv","__pycache__","wandb","runs","debug",".git","datasets",
             "TFG_S_big_newdis_v3_pt_fix_MutiLoss") +
           (Get-ChildItem $Workspace -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^(build|logs|outputs)' } |
            ForEach-Object { $_.Name })
$ExFiles = @("*.pyc","*.tar","*.wav","*.onnx","*.pt","*.ckpt","err.txt","*.sarif")

Write-Host "[sync] $Workspace -> $BridgeCode"
Write-Host "[sync] excluding dirs: $($ExDirs -join ', ')"

robocopy $Workspace $BridgeCode `
    /MIR `
    /XD @ExDirs `
    /XF @ExFiles `
    /NFL /NDL /NJH /NP `
    /R:2 /W:2 | Out-Host

$rc = $LASTEXITCODE
# robocopy: exit codes 0-7 are success (see `robocopy /?`), >=8 is real failure.
if ($rc -ge 8) { Write-Host "robocopy failed with code $rc" -ForegroundColor Red; exit $rc }
Write-Host "[sync] done (robocopy exit $rc)"
