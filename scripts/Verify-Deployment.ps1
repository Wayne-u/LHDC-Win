$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$projectRoot=Split-Path -Parent $PSScriptRoot
$deviceTool=Join-Path $projectRoot 'build\host\lhdc-device.exe'
$package=Join-Path $projectRoot 'build\test-package'
$certificate=Join-Path $projectRoot 'build\test-certificate\lhdc-test.cer'
$output=Join-Path $projectRoot ('evidence\local\deployment-verification-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $output -Force | Out-Null
$scratch=Join-Path $output 'scratch'
New-Item -ItemType Directory -Path $scratch | Out-Null
$cases=[Collections.Generic.List[object]]::new()
function Test-Command([string]$Name,[string[]]$Arguments,[int]$ExpectedExitCode) {
    $lines=@(& $deviceTool @Arguments 2>&1 | ForEach-Object { $_.ToString() })
    $code=$LASTEXITCODE
    $lines | Set-Content -LiteralPath (Join-Path $output ($Name+'.jsonl')) -Encoding UTF8
    if ($code -ne $ExpectedExitCode) { throw "Unexpected exit $code for $Name; expected $ExpectedExitCode. See $output." }
    $cases.Add([pscustomobject]@{Name=$Name;ExitCode=$code;Records=@($lines | ForEach-Object { $_ | ConvertFrom-Json })})
}
function Copy-TestPackage([string]$Name) {
    $directory=Join-Path $scratch $Name
    New-Item -ItemType Directory -Path $directory | Out-Null
    foreach ($file in @('lhdc-transport.sys','lhdc-transport.inf','lhdc-transport.cat')) {
        Copy-Item -LiteralPath (Join-Path $package $file) -Destination $directory
    }
    $directory
}
try {
    Test-Command 'valid-package' @('verify-package',$package,$certificate) 0
    $sysDirectory=Copy-TestPackage 'tampered-sys'
    $sys=Join-Path $sysDirectory 'lhdc-transport.sys'
    $bytes=[IO.File]::ReadAllBytes($sys)
    $peOffset=[BitConverter]::ToInt32($bytes,0x3c)
    $optionalHeaderSize=[BitConverter]::ToUInt16($bytes,$peOffset+20)
    $firstSection=$peOffset+24+$optionalHeaderSize
    $rawOffset=[BitConverter]::ToInt32($bytes,$firstSection+20)
    if ($rawOffset -lt $firstSection+40 -or $rawOffset -ge $bytes.Length) { throw 'Unexpected test SYS section offset.' }
    $bytes[$rawOffset]=$bytes[$rawOffset] -bxor 1
    [IO.File]::WriteAllBytes($sys,$bytes)
    Test-Command 'reject-sys-code-tamper' @('verify-package',$sysDirectory,$certificate) 1
    $infDirectory=Copy-TestPackage 'tampered-inf'
    Add-Content -LiteralPath (Join-Path $infDirectory 'lhdc-transport.inf') -Value '; tampered after signing' -Encoding UTF8
    Test-Command 'reject-inf-tamper' @('verify-package',$infDirectory,$certificate) 1
    $catDirectory=Copy-TestPackage 'tampered-cat'
    $cat=Join-Path $catDirectory 'lhdc-transport.cat'
    $bytes=[IO.File]::ReadAllBytes($cat)
    $bytes[$bytes.Length-1]=$bytes[$bytes.Length-1] -bxor 1
    [IO.File]::WriteAllBytes($cat,$bytes)
    Test-Command 'reject-cat-signature-tamper' @('verify-package',$catDirectory,$certificate) 1
    $differentCertificate=Get-ChildItem -LiteralPath 'Cert:\LocalMachine\Root' |
        Where-Object { $_.NotBefore -lt (Get-Date) -and $_.NotAfter -gt (Get-Date) } | Select-Object -First 1
    if (-not $differentCertificate) { throw 'No valid system certificate is available for the wrong-key test.' }
    $wrongCertificate=Join-Path $scratch 'wrong-public-key.cer'
    Export-Certificate -Cert $differentCertificate -FilePath $wrongCertificate -Type CERT | Out-Null
    Test-Command 'reject-wrong-signer' @('verify-package',$package,$wrongCertificate) 1
    $wrongInstance='BTHENUM\NOT-THE-RECORDED-ENCO-INSTANCE'
    $inf=Join-Path $package 'lhdc-transport.inf'
    Test-Command 'reject-other-snapshot' @('snapshot',$wrongInstance) 1
    Test-Command 'reject-other-candidates' @('candidates',$wrongInstance,$inf) 1
    Test-Command 'reject-other-install' @('install',$wrongInstance,$inf,$certificate) 1
    Test-Command 'reject-other-restore' @('restore',$wrongInstance) 1
    $stateText=& $deviceTool readiness
    if ($LASTEXITCODE -ne 0) { throw 'Cannot read deployment prerequisites.' }
    $state=$stateText | ConvertFrom-Json
    $discovery=@(& (Join-Path $projectRoot 'build/host/lhdc-host.exe') target)
    if($LASTEXITCODE -ne 0) { throw 'Target discovery failed.' }
    $target=$discovery[-1] | ConvertFrom-Json
    if ($target.found -and (-not $state.administrator -or -not $state.test_signing_active)) {
        # This test is only safe while a mandatory, non-mutating guard is false.
        Test-Command 'reject-install-without-prerequisites' @('install',$target.instance,$inf,$certificate) 1
    }
    [pscustomobject]@{VerifiedAt=(Get-Date -Format o);DriverInstallPerformed=$false;BootConfigurationChanged=$false;TrustStoresChanged=$false;Readiness=$state;Cases=$cases.ToArray()} |
        ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding UTF8
} finally {
    $resolvedScratch=(Resolve-Path -LiteralPath $scratch).Path
    if ($resolvedScratch -ne (Join-Path $output 'scratch') -or -not $resolvedScratch.StartsWith($projectRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
        throw 'Scratch directory is outside the verification output.'
    }
    Remove-Item -LiteralPath $resolvedScratch -Recurse -Force
}
Write-Host "Passed $($cases.Count) deployment checks. Evidence: $output"
