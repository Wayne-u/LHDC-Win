param([ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
$output=Join-Path $projectRoot 'build\ui'
dotnet build (Join-Path $projectRoot 'ui\Lhdc.ControlPanel\Lhdc.ControlPanel.csproj') -c $Configuration -o $output --nologo
if ($LASTEXITCODE -ne 0) { throw 'Control panel build failed.' }
[pscustomobject]@{WorkspaceRoot=$projectRoot;PowerShellExecutable=(Get-Process -Id $PID).Path} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'workspace.json') -Encoding UTF8
Write-Host "Control panel: $output\LHDC-ControlPanel.exe"
