param([switch]$Driver,[switch]$Analyze, [ValidateSet('Debug','Release')][string]$Configuration='Release')
$ErrorActionPreference='Stop'
$projectRoot=Split-Path -Parent $PSScriptRoot
$vswhere='C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$installation=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'MSVC x64 toolchain was not found.' }
Import-Module "$installation\Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
if ($Driver) {
    $analysisArguments=@()
    if ($Analyze) {
        $kitsRoot=(Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots').KitsRoot10
        $rules=Join-Path $kitsRoot 'CodeAnalysis\DriverMinimumRules.ruleset'
        $analysisArguments=@('/p:RunCodeAnalysis=true',"/p:CodeAnalysisRuleSet=$rules")
    }
    & "$installation\MSBuild\Current\Bin\MSBuild.exe" "$projectRoot\drivers\transport\lhdc-transport.vcxproj" /m "/p:Configuration=$Configuration" /p:Platform=x64 /verbosity:minimal @analysisArguments
    if ($LASTEXITCODE -ne 0) { throw 'Driver build failed.' }
} else {
    cmake -S $projectRoot -B "$projectRoot\build\host" -G Ninja "-DCMAKE_BUILD_TYPE=$Configuration" -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl
    if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
    cmake --build "$projectRoot\build\host"
    if ($LASTEXITCODE -ne 0) { throw 'Host build failed.' }
    ctest --test-dir "$projectRoot\build\host" --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
