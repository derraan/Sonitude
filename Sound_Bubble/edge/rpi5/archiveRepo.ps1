param(
  [string]$SrcRoot = 'C:\Users\darre',
  [string]$SrcDirName = 'Sound_Bubble',
  [string]$OutDir = 'C:\Users\darre\Documents\Obsidian Notes\CDE3301\3301-ANCHRTF',
  [string]$ArchiveName = 'Sound_Bubble_code_and_models_2.tar.gz'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$srcDir = Join-Path $SrcRoot $SrcDirName
if (-not (Test-Path -LiteralPath $srcDir)) {
  throw "Source directory not found: $srcDir"
}

$outDirFull = if ([System.IO.Path]::IsPathRooted($OutDir)) { $OutDir } else { (Join-Path $SrcRoot $OutDir) }
New-Item -ItemType Directory -Force -Path $outDirFull | Out-Null
$archivePath = Join-Path $outDirFull $ArchiveName

Write-Host "[archive] creating: $archivePath"
Write-Host "[archive] output_dir: $outDirFull"

& tar -czvf $archivePath `
  --exclude=".git" `
  --exclude=".venv" `
  --exclude="__pycache__" `
  --exclude="*.pyc" `
  --exclude="runs" `
  --exclude="datasets" `
  -C $SrcRoot $SrcDirName

Write-Host ""
Write-Host "[archive] first 20 entries:"
& tar -tzf $archivePath | Select-Object -First 20

