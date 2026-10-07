param([Parameter(Mandatory)][ValidateSet('Enable','Disable')][string]$Action)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$projectRoot=Split-Path -Parent $PSScriptRoot
$deviceTool=Join-Path $projectRoot 'build\host\lhdc-device.exe'
$readinessText=& $deviceTool readiness
if ($LASTEXITCODE -ne 0) { throw 'Cannot read the running kernel signing state.' }
$readiness=$readinessText | ConvertFrom-Json
if (-not $readiness.administrator) { throw 'This action requires an elevated administrator PowerShell process.' }
$evidence=Join-Path $projectRoot 'evidence\local'
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$operation=[ordered]@{AttemptedAt=(Get-Date -Format o);Action=$Action;RunningKernelBefore=$readiness;Completed=$false;BootConfigurationChanged=$false;RestartRequired=$false;RestartInitiated=$false;Error=$null}
try {
    if ($Action -eq 'Enable') {
        $secureBoot=Confirm-SecureBootUEFI
        if ($secureBoot) { throw 'Secure Boot is enabled. This script will not modify Secure Boot or other protections.' }
    }
    $bcdedit=Join-Path $env:SystemRoot 'System32\bcdedit.exe'
    $before=& $bcdedit /enum
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read BCD; no changes were made.' }
    $before | Set-Content -LiteralPath (Join-Path $evidence "bcd-before-$Action.txt") -Encoding UTF8
    $value=if ($Action -eq 'Enable') { 'ON' } else { 'OFF' }
    $change=& $bcdedit /set TESTSIGNING $value 2>&1 | ForEach-Object { $_.ToString() }
    $changeExitCode=$LASTEXITCODE
    $change | Set-Content -LiteralPath (Join-Path $evidence "bcd-change-$Action.txt") -Encoding UTF8
    if ($changeExitCode -ne 0) { throw "BCDEdit rejected TESTSIGNING $value." }
    $operation.BootConfigurationChanged=$true
    $operation.RestartRequired=$true
    $after=& $bcdedit /enum
    if ($LASTEXITCODE -ne 0) { throw 'TESTSIGNING was changed, but the resulting BCD read failed. Inspect the saved before-state.' }
    $after | Set-Content -LiteralPath (Join-Path $evidence "bcd-after-$Action.txt") -Encoding UTF8
    $operation.Completed=$true
} catch {
    $operation.Error=$_.Exception.Message
    throw
} finally {
    $operation.FinishedAt=Get-Date -Format o
    $operation | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $evidence "test-signing-$Action.json") -Encoding UTF8
}
Write-Host "TESTSIGNING is now $value in BCD. Restart is required; this script does not restart Windows."
