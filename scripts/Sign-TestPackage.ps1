param([switch]$Audio)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$projectRoot=Split-Path -Parent $PSScriptRoot
$packageName=if ($Audio) { 'lhdc-audio' } else { 'lhdc-transport' }
$source=if ($Audio) { Join-Path $projectRoot 'build/audio-driver/lhdc-audio' } else { Join-Path $projectRoot 'build/driver/Release/lhdc-transport' }
$destination=Join-Path $projectRoot $(if ($Audio) { 'build/test-audio-package' } else { 'build/test-package' })
if ($Audio) {
    $ready=Get-Content -LiteralPath (Join-Path $projectRoot 'build/audio-driver/build-ready.json') -Encoding UTF8 | ConvertFrom-Json
    foreach ($file in $ready.Files) {
        if ((Get-FileHash -LiteralPath $file.Path -Algorithm SHA256).Hash -ne $file.Hash) { throw 'Audio package differs from the completed build.' }
    }
}
$certDirectory=Join-Path $projectRoot 'build\test-certificate'
$kitsRoot=(Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
$signTool=Join-Path $kitsRoot 'bin\10.0.28000.0\x64\signtool.exe'
$inf2cat=Join-Path $kitsRoot 'bin\10.0.28000.0\x86\Inf2Cat.exe'
New-Item -ItemType Directory -Path $destination,$certDirectory -Force | Out-Null
$metadata=Join-Path $certDirectory 'certificate.json'
if (Test-Path -LiteralPath $metadata) {
    $saved=Get-Content -LiteralPath $metadata -Encoding UTF8 | ConvertFrom-Json
    $certificate=Get-Item -LiteralPath "Cert:\CurrentUser\My\$($saved.Thumbprint)"
} else {
    $certificate=New-SelfSignedCertificate -Subject 'CN=Windows LHDC M2 Test' -Type CodeSigningCert -CertStoreLocation 'Cert:\CurrentUser\My' -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(1)
    [pscustomobject]@{Thumbprint=$certificate.Thumbprint;Subject=$certificate.Subject;Store='CurrentUser/My';PrivateKeyExported=$false} |
        ConvertTo-Json | Set-Content -LiteralPath $metadata -Encoding UTF8
}
if (-not $certificate.HasPrivateKey -or $certificate.NotAfter -le (Get-Date)) { throw 'The saved signing certificate has no usable private key or has expired.' }
$publicCertificate=Join-Path $certDirectory 'lhdc-test.cer'
Export-Certificate -Cert $certificate -FilePath $publicCertificate -Type CERT | Out-Null
Copy-Item -LiteralPath (Join-Path $source ($packageName+'.sys')),(Join-Path $source ($packageName+'.inf')) -Destination $destination -Force
& $signTool sign /fd SHA256 /s My /sha1 $certificate.Thumbprint (Join-Path $destination ($packageName+'.sys'))
if ($LASTEXITCODE -ne 0) { throw 'SYS signing failed.' }
# Generate the catalog after signing SYS so its hash covers the signed binary.
& $inf2cat "/driver:$destination" /os:10_GE_X64
if ($LASTEXITCODE -ne 0) { throw 'Catalog regeneration failed.' }
& $signTool sign /fd SHA256 /s My /sha1 $certificate.Thumbprint (Join-Path $destination ($packageName+'.cat'))
if ($LASTEXITCODE -ne 0) { throw 'Catalog signing failed.' }
$verifier=Join-Path $projectRoot $(if ($Audio) { 'build/host/lhdc-audio-device.exe' } else { 'build/host/lhdc-device.exe' })
$verificationText=& $verifier verify-package $destination $publicCertificate
if ($LASTEXITCODE -ne 0) { throw 'Test package signature or file digest verification failed.' }
$verification=$verificationText | ConvertFrom-Json
if ($verification.certificate_thumbprint -ne $certificate.Thumbprint) { throw 'Verified certificate differs from the saved signing certificate.' }
$evidence=Join-Path $projectRoot $(if ($Audio) { 'evidence/local/audio-test-package.json' } else { 'evidence/local/test-package.json' })
New-Item -ItemType Directory -Path (Split-Path -Parent $evidence) -Force | Out-Null
[pscustomobject]@{
    SignedAt=(Get-Date -Format o);Package=$destination;Certificate=$publicCertificate;Thumbprint=$certificate.Thumbprint
    Verification=$verification;Hashes=@(Get-ChildItem -LiteralPath $destination -File | Get-FileHash -Algorithm SHA256 | Select-Object Path,Hash)
    TrustStoresChanged=$false
    MachineTrustChanged=$false;BootConfigurationChanged=$false;DriverInstalled=$false
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $evidence -Encoding UTF8
Write-Host "Signed test package: $destination. Public certificate: $publicCertificate. Machine trust and boot settings were not changed."
