param(
    [ValidateSet('Debug','Release')][string]$Configuration='Release',
    [string]$PowerShellDirectory,
    [string]$OutputDirectory='build/app',
    [switch]$NoArchive
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$projectRoot=Split-Path -Parent $PSScriptRoot
$sourceCommit=& git -C $projectRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the release source commit.' }
$sourceStatus=@(& git -C $projectRoot status --porcelain)
if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect the release source status.' }
$buildRoot=[IO.Path]::GetFullPath((Join-Path $projectRoot 'build'))
$output=[IO.Path]::GetFullPath($OutputDirectory,$projectRoot).TrimEnd([IO.Path]::DirectorySeparatorChar)
if ((Split-Path -Parent $output) -ne $buildRoot -or (Split-Path -Leaf $output) -notmatch '^app(?:-[A-Za-z0-9][A-Za-z0-9._-]*)?$') {
    throw 'Package output must be build/app or a build/app-* directory.'
}
foreach ($path in @($buildRoot,$output)) {
    if ((Test-Path -LiteralPath $path) -and ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Package output cannot use a directory link.' }
}
$uiProject=Join-Path $projectRoot 'ui/Lhdc.ControlPanel/Lhdc.ControlPanel.csproj'
if ($Configuration -eq 'Debug' -and -not $NoArchive) { throw 'ZIP packages must use Release configuration.' }
if (-not $PowerShellDirectory) {
    $installed=Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/Microsoft/PowerShellCore/InstalledVersions/31ab5147-9a97-4452-8443-d9709f0516e1'
    $PowerShellDirectory=$installed.InstallLocation
}
$PowerShellDirectory=(Resolve-Path -LiteralPath $PowerShellDirectory).Path
if ($PowerShellDirectory -eq $output -or $PowerShellDirectory.StartsWith($output+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'PowerShell input cannot be inside the package output directory.' }
$pwsh=Join-Path $PowerShellDirectory 'pwsh.exe'
foreach ($name in @('pwsh.exe','pwsh.dll','coreclr.dll','hostfxr.dll','LICENSE.txt','ThirdPartyNotices.txt')) {
    if (-not (Test-Path -LiteralPath (Join-Path $PowerShellDirectory $name) -PathType Leaf)) { throw "PowerShell runtime is incomplete: $name" }
}
$runtimeText=& $pwsh -NoProfile -Command '$PSVersionTable.PSVersion.ToString(); [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()'
if ($LASTEXITCODE -ne 0 -or $runtimeText.Count -ne 2 -or ([version]$runtimeText[0]).Major -ne 7 -or $runtimeText[1] -ne 'X64') {
    throw 'The bundled runtime must be PowerShell 7 x64.'
}
$runtimeConfig=Get-Content -LiteralPath (Join-Path $PowerShellDirectory 'pwsh.runtimeconfig.json') -Raw -Encoding UTF8 | ConvertFrom-Json
if ($runtimeConfig.runtimeOptions.PSObject.Properties.Name -contains 'frameworks' -or
    $runtimeConfig.runtimeOptions.PSObject.Properties.Name -contains 'framework') { throw 'PowerShell must include its own .NET runtime.' }
foreach ($path in @('build/test-certificate/lhdc-test.cer','build/test-package/lhdc-transport.inf','build/test-package/lhdc-transport.sys','build/test-package/lhdc-transport.cat','build/test-audio-package/lhdc-audio.inf','build/test-audio-package/lhdc-audio.sys','build/test-audio-package/lhdc-audio.cat')) {
    if (-not (Test-Path -LiteralPath (Join-Path $projectRoot $path) -PathType Leaf)) { throw "Build and sign the drivers before publishing: $path" }
}
$running=@(Get-CimInstance Win32_Process -Filter "Name='LHDC-ControlPanel.exe'" | Where-Object ExecutablePath -EQ (Join-Path $output 'LHDC-ControlPanel.exe'))
if ($running.Count) { throw "Close the control panel in $output before publishing." }

& (Join-Path $PSScriptRoot 'Build.ps1') -Target Host -Configuration $Configuration -WithoutTests
$resolvedOutput=[IO.Path]::GetFullPath($output)
if ((Split-Path -Parent $resolvedOutput) -ne $buildRoot -or ((Get-Item -LiteralPath $buildRoot).Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Package output is outside the workspace build directory.' }
if (Test-Path -LiteralPath $resolvedOutput) {
    if ((Get-Item -LiteralPath $resolvedOutput).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Package output cannot be a directory link.' }
    Remove-Item -LiteralPath $resolvedOutput -Recurse -Force
}
New-Item -ItemType Directory -Path $output | Out-Null
dotnet publish $uiProject -c $Configuration -r win-x64 --self-contained true -o $output --nologo -p:DebugType=None -p:DebugSymbols=false
if ($LASTEXITCODE -ne 0) { throw 'Self-contained control panel publication failed.' }

foreach ($directory in @('backend','drivers/transport','drivers/audio','runtime/powershell','scripts','licenses')) {
    New-Item -ItemType Directory -Path (Join-Path $output $directory) | Out-Null
}
$nativeDependencies=foreach ($name in @('lhdc-host','lhdc-service','lhdc-device','lhdc-audio-device')) {
    $program=Join-Path $projectRoot ('build/host/'+$name+'.exe')
    $dependencies=& dumpbin.exe /dependents $program
    if ($LASTEXITCODE -ne 0) { throw "Cannot inspect native dependencies: $name" }
    if ($dependencies -match '(?i)\b(?:msvcp\d+\w*|vcruntime\d+\w*|msvcr\d+|ucrtbase)\.dll\b') { throw "Native runtime was not statically linked: $name" }
    Copy-Item -LiteralPath $program -Destination (Join-Path $output 'backend')
    [pscustomobject]@{Program=$name;Dependencies=@($dependencies | Where-Object { $_ -match '(?i)\.(dll|cpl)\s*$' } | ForEach-Object Trim)}
}
foreach ($driver in @(@{Name='lhdc-transport';Source='build/test-package';Destination='drivers/transport';Verifier='lhdc-device'},@{Name='lhdc-audio';Source='build/test-audio-package';Destination='drivers/audio';Verifier='lhdc-audio-device'})) {
    foreach ($extension in @('sys','inf','cat')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot ($driver.Source+'/'+$driver.Name+'.'+$extension)) -Destination (Join-Path $output $driver.Destination)
    }
    $verification=& (Join-Path $output ('backend/'+$driver.Verifier+'.exe')) verify-package (Join-Path $output $driver.Destination) (Join-Path $projectRoot 'build/test-certificate/lhdc-test.cer')
    if ($LASTEXITCODE -ne 0) { throw "Driver signature or catalog verification failed: $($driver.Name)" }
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'build/test-certificate/lhdc-test.cer') -Destination (Join-Path $output 'drivers/lhdc-test.cer')
Get-ChildItem -LiteralPath $PowerShellDirectory -Force | Copy-Item -Destination (Join-Path $output 'runtime/powershell') -Recurse
Get-ChildItem -LiteralPath (Join-Path $output 'runtime/powershell') -Recurse -File -Filter '*.pdb' | ForEach-Object { Remove-Item -LiteralPath $_.FullName }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Set-AudioMode.ps1') -Destination (Join-Path $output 'scripts')
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE'),(Join-Path $projectRoot 'NOTICE.md') -Destination $output
Get-ChildItem -LiteralPath (Join-Path $projectRoot 'licenses') -File | Copy-Item -Destination (Join-Path $output 'licenses')
Copy-Item -LiteralPath (Join-Path $PowerShellDirectory 'LICENSE.txt') -Destination (Join-Path $output 'licenses/PowerShell-LICENSE.txt')
Copy-Item -LiteralPath (Join-Path $PowerShellDirectory 'ThirdPartyNotices.txt') -Destination (Join-Path $output 'licenses/PowerShell-ThirdPartyNotices.txt')
$runtimeItems=& dotnet msbuild $uiProject -target:ResolveReferences -getItem:ResolvedRuntimePack "-property:Configuration=$Configuration" -nologo
if ($LASTEXITCODE -ne 0) { throw '.NET runtime pack inspection failed.' }
$runtimePacks=@(($runtimeItems | ConvertFrom-Json).Items.ResolvedRuntimePack | Where-Object FrameworkName -In 'Microsoft.NETCore.App','Microsoft.WindowsDesktop.App')
if ($runtimePacks.Count -ne 2) { throw 'Expected .NET and Windows Desktop runtime packs.' }
foreach ($pack in $runtimePacks) {
    $licenseFiles=@(Get-ChildItem -LiteralPath $pack.PackageDirectory -File | Where-Object Name -Match '^(LICENSE|THIRD-PARTY-NOTICES)')
    if (-not $licenseFiles.Count) { throw "Runtime license is missing: $($pack.FrameworkName)" }
    foreach ($file in $licenseFiles) {
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $output ('licenses/'+$pack.FrameworkName+'-'+$file.Name))
    }
}

@'
Windows LHDC 使用说明

1. 将整个压缩包解压到一个文件夹，双击 LHDC-ControlPanel.exe。
2. 在 Windows 中配对支持 LHDC V5 的蓝牙耳机，并连接电脑。
3. 在面板选择耳机，再选择 LHDC 并应用；安装时校验真实能力，不兼容则恢复 Windows 音频。首次安装需要管理员授权。
4. 恢复 Windows 标准音频时，在面板选择 Windows 并应用。

要求：Windows 11 22H2 或以后版本（构建号 22621+），Intel/AMD x64 电脑。
本包已内置 .NET、PowerShell 和原生运行依赖，无需另行安装运行库。
本包仍使用测试签名驱动，测试模式及现有驱动加载条件须事先满足；程序不自动修改启动配置、Secure Boot 或内存完整性设置。
面板及部署日志保存在用户的 LocalAppData/LHDC-Win；服务和恢复状态保存在系统 ProgramData/LHDC-Win。
安装时按耳机实际能力选择 AVRCP 统一音量或 Windows 软件音量，并按设备记忆音量和静音。软件音量模式下耳机自身增益独立。Hi-Res 控制只对已知兼容型号显示，不是播放前提。关闭面板不影响后台播放。
请保留完整解压目录。通用播放模式尚待不同型号耳机实测；此包尚未取得正式驱动签名，其他 Windows 构建和蓝牙适配器的兼容性仍需实测。
'@ | Set-Content -LiteralPath (Join-Path $output '使用说明.txt') -Encoding UTF8
$projectXml=[xml](Get-Content -LiteralPath $uiProject -Raw -Encoding UTF8)
$version=[string]$projectXml.Project.PropertyGroup.Version
$manifest=[ordered]@{Version=$version;SourceCommit=$sourceCommit;SourceTreeDirty=$sourceStatus.Count -gt 0;BuiltAt=Get-Date -Format o;Configuration=$Configuration;Architecture='x64';MinimumWindowsBuild=22621;DriverSigning='Test';TestSigningRequired=$true;SelfContainedUI=$true;DotNetRuntimes=@($runtimePacks | Select-Object FrameworkName,NuGetPackageVersion);PowerShellVersion=$runtimeText[0];StaticNativeRuntime=$true;NativeDependencies=$nativeDependencies}
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $output 'release.json') -Encoding UTF8
$hashes=foreach ($file in Get-ChildItem -LiteralPath $output -Recurse -File) {
    [pscustomobject]@{Path=[IO.Path]::GetRelativePath($output,$file.FullName).Replace('\','/');SHA256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash}
}
$hashes | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $output 'checksums.json') -Encoding UTF8
if (-not $NoArchive) {
    $archive=Join-Path $projectRoot ('build/LHDC-Win-'+$version+'-win-x64-test.zip')
    if (Test-Path -LiteralPath $archive) { Remove-Item -LiteralPath $archive }
    [IO.Compression.ZipFile]::CreateFromDirectory($output,$archive,[IO.Compression.CompressionLevel]::Optimal,$false)
    $digest=Get-FileHash -LiteralPath $archive -Algorithm SHA256
    ($digest.Hash+'  '+[IO.Path]::GetFileName($archive)) | Set-Content -LiteralPath ($archive+'.sha256') -Encoding UTF8
    Write-Host "ZIP package: $archive"
}
Write-Host "Application directory: $output"
