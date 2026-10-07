$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
$program=Join-Path $projectRoot 'build\ui\LHDC-ControlPanel.exe'
if (-not (Test-Path -LiteralPath $program)) { throw 'Build the panel first with scripts/Build-UI.ps1.' }
Start-Process -FilePath $program
