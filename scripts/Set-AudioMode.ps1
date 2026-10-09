param(
    [ValidateSet('LHDC','Windows')][string]$Mode='LHDC',
    [ValidateSet(44100,48000,96000,192000)][int]$SampleRate=48000,
    [ValidateSet(16,24)][int]$Bits=24,
    [int]$Kbps=400,
    [switch]$AdaptiveBitrate,
    [string]$ResultFile
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$OutputEncoding=[Console]::OutputEncoding
$projectRoot=Split-Path -Parent $PSScriptRoot
$evidence=Join-Path $projectRoot ('evidence/local/integrated-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+$Mode)
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$result=[ordered]@{Mode=$Mode;StartedAt=Get-Date -Format o;EvidenceDirectory=$evidence;Completed=$false;Error=$null;RecoveryError=$null}
$deviceTool=Join-Path $projectRoot 'build/host/lhdc-device.exe'
$audioTool=Join-Path $projectRoot 'build/host/lhdc-audio-device.exe'
$hostTool=Join-Path $projectRoot 'build/host/lhdc-host.exe'
$deviceText=@(& $hostTool target)
if($LASTEXITCODE -ne 0) { throw 'Target device discovery failed.' }
$device=$deviceText[-1] | ConvertFrom-Json
if(-not $device.found) { throw 'No supported Enco X4 is paired.' }
$target=$device.instance
function Invoke-Native([string]$Tool,[string[]]$Arguments,[string]$Name,[switch]$AllowRestart) {
    $output=@(& $Tool @Arguments 2>&1)
    $code=$LASTEXITCODE
    $output | Set-Content -LiteralPath (Join-Path $evidence ($Name+'.jsonl')) -Encoding UTF8
    if ($code -ne 0 -and -not ($AllowRestart -and $code -eq 3)) { throw "$Name failed (exit $code); see $evidence." }
    $record=$output[-1] | ConvertFrom-Json
    if ($code -eq 3) {
        if (-not $record.binding_verified -or -not $record.reboot_required) { throw 'Pending driver restart did not verify its binding.' }
        $result.RebootRequired=$true
    }
    return $record
}
function Restore-InboxAudio([string]$Name) {
    if ($null -ne (Get-LhdcService)) { Set-Service -Name 'LHDC-Win' -StartupType Disabled }
    $children=@(Get-PnpDevice -PresentOnly | Where-Object InstanceId -Like 'LHDCWIN\AUDIO\*')
    foreach($child in $children) {
        $parent=(Get-PnpDeviceProperty -InstanceId $child.InstanceId -KeyName DEVPKEY_Device_Parent).Data
        if ($parent -ne $target) { throw 'Refusing to remove an audio child belonging to a different device.' }
        $output=@(& pnputil.exe /remove-device $child.InstanceId /subtree 2>&1)
        $code=$LASTEXITCODE
        $output | Set-Content -LiteralPath (Join-Path $evidence ($Name+'-child-removal.txt')) -Encoding UTF8
        if ($code -eq 3010) { $result.RebootRequired=$true }
        elseif ($code -ne 0) { throw "The LHDC audio child could not be removed, exit=$code." }
    }
    $record=Invoke-Native $deviceTool @('restore',$target) $Name -AllowRestart
    $script:bound=$false
    if ($record.reboot_required -or $result.Contains('RebootRequired')) { throw 'Windows 驱动已绑定，但设备仍需重启才能完成切换。请手动重启电脑；尚未确认立体声恢复。' }
    return $record
}
function Get-LhdcService {
    $service=Get-CimInstance -ClassName Win32_Service -Filter "Name='LHDC-Win'"
    $expected=Join-Path $env:ProgramFiles 'LHDC-Win/lhdc-service.exe'
    if ($null -ne $service -and ($service.PathName -ne ('"'+$expected+'"') -or $service.StartName -ne 'LocalSystem')) { throw 'Existing LHDC service configuration differs.' }
    return $service
}
function Stop-LhdcService {
    $service=Get-LhdcService
    if ($null -ne $service -and $service.State -ne 'Stopped') {
        Stop-Service -Name 'LHDC-Win'
        (Get-Service -Name 'LHDC-Win').WaitForStatus('Stopped',[TimeSpan]::FromSeconds(20))
    }
}
function Restart-LhdcAudioChild {
    $children=@(Get-PnpDevice -PresentOnly | Where-Object InstanceId -Like 'LHDCWIN\AUDIO\*')
    if ($children.Count -ne 1) { throw 'Expected one LHDC audio child for a format change.' }
    $parent=(Get-PnpDeviceProperty -InstanceId $children[0].InstanceId -KeyName DEVPKEY_Device_Parent).Data
    if ($parent -ne $target) { throw 'Refusing to restart an audio child belonging to a different device.' }
    $output=@(& pnputil.exe /restart-device $children[0].InstanceId 2>&1)
    $code=$LASTEXITCODE
    $output | Set-Content -LiteralPath (Join-Path $evidence 'format-restart.txt') -Encoding UTF8
    if ($code -eq 3010) { $result.RebootRequired=$true; throw '播放格式切换需要手动重启电脑，尚未完成。' }
    if ($code -ne 0) { throw "Audio child restart failed, exit=$code." }
    $result.FormatRestarted=$true
}
function Wait-LhdcPlaybackFormat {
    $connection=Invoke-Native $hostTool @('connection',$device.address) 'format-connection'
    if (-not $connection.connected) { $result.FormatVerificationPending=$true; return }
    $inputBits=$Bits
    $deadline=(Get-Date).AddSeconds(20)
    $stableId=$null
    $stableSince=$null
    $stabilityWindow=[TimeSpan]::FromSeconds(2)
    do {
        $devices=Invoke-Native $hostTool @('audio-devices') 'format-endpoints'
        $endpoint=@($devices.devices | Where-Object { $_.adapter_id -like '*lhdcwin#audio#*' -and $_.channels -eq 2 -and $_.sample_rate -eq $SampleRate -and $_.device_sample_rate -eq $SampleRate -and $_.device_bits -eq $inputBits })
        if ($endpoint.Count -eq 1) {
            $ready=Invoke-Native $hostTool @('playback-ready',$endpoint[0].id,"$SampleRate","$Bits") 'format-readiness'
            if ($ready.ready) {
                if ($stableId -ne $endpoint[0].id) { $stableId=$endpoint[0].id; $stableSince=Get-Date }
                # Endpoint Builder can briefly remove an otherwise valid new
                # endpoint while Bluetooth unification updates its routing.
                if (((Get-Date)-$stableSince) -ge $stabilityWindow) {
                    $result.PlaybackFormatVerified=$endpoint[0]
                    $result.PlaybackClientVerified=$true
                    return
                }
            } else { $stableId=$null; $stableSince=$null }
        } else { $stableId=$null; $stableSince=$null }
        [Threading.Thread]::Sleep(250)
    } while ((Get-Date) -lt $deadline)
    throw '所选播放格式未在 Windows 端点生效，切换未通过验证。'
}
function Write-CodecProfile {
    $registry=[Microsoft.Win32.Registry]::LocalMachine.CreateSubKey('SOFTWARE\LHDC-Win')
    try {
        $script:previousProfile=$registry.GetValue('Profile')
        $script:previousAdaptive=$registry.GetValue('AdaptiveBitrate')
        [byte[]]$profileBytes=@([BitConverter]::GetBytes($SampleRate)+[BitConverter]::GetBytes($Bits)+[BitConverter]::GetBytes($Kbps))
        $registry.SetValue('Profile',$profileBytes,[Microsoft.Win32.RegistryValueKind]::Binary)
        $registry.SetValue('AdaptiveBitrate',[int][bool]$AdaptiveBitrate,[Microsoft.Win32.RegistryValueKind]::DWord)
    } finally { $registry.Dispose() }
    # Grant normal users only data-value writes on this codec-only key. The
    # service independently validates the fixed profile before using it.
    $acl=Get-Acl -LiteralPath 'HKLM:\SOFTWARE\LHDC-Win'
    $rule=[Security.AccessControl.RegistryAccessRule]::new([Security.Principal.SecurityIdentifier]::new('S-1-5-32-545'),'SetValue,QueryValues','None','None','Allow')
    $acl.AddAccessRule($rule)
    # Set-Acl -LiteralPath does not resolve this registry path in PowerShell 7.6.
    Set-Acl -Path 'HKLM:\SOFTWARE\LHDC-Win' -AclObject $acl
}
$bound=$false
$previousProfile=$null
$previousAdaptive=$null
try {
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Audio mode installation requires an administrator process.' }
    $result.Before=Invoke-Native $deviceTool @('snapshot',$target) 'before'
    if ($result.Before.service -notin @('BthA2dp','lhdc-transport')) { throw 'The target has an unrelated audio driver.' }
    if ($Mode -eq 'LHDC' -and ($SampleRate -gt 48000 -or $Kbps -gt 400)) {
        $hires=Invoke-Native $hostTool @('hires') 'hires-state'
        if (-not $hires.supported -or $hires.enabled -ne $true) { throw 'Enable and verify earbud Hi-Res before selecting a high-rate LHDC profile.' }
    }
    if ($Mode -eq 'Windows') {
        $bound=$result.Before.service -eq 'lhdc-transport'
        Stop-LhdcService
        $result.Restore=Restore-InboxAudio 'restore'
        $result.After=Invoke-Native $deviceTool @('snapshot',$target) 'after'
        if ($result.After.service -ne 'BthA2dp' -or $result.After.problem -ne 0) { throw 'Windows A2DP restoration did not verify.' }
        $connection=Invoke-Native $hostTool @('connection',$device.address) 'windows-connection'
        if ($connection.connected) {
            $deadline=(Get-Date).AddSeconds(15)
            do {
                $devices=Invoke-Native $hostTool @('audio-devices') 'windows-endpoints'
                $stereo=@($devices.devices | Where-Object { $_.name -like '*Enco X4*' -and $_.name -notlike '*LHDC*' -and $_.channels -eq 2 -and $_.device_sample_rate -ge 44100 })
                if ($stereo.Count -eq 1) { break }
                [Threading.Thread]::Sleep(250)
            } while ((Get-Date) -lt $deadline)
            if ($stereo.Count -ne 1) { throw 'Windows 原生驱动已绑定，但双声道立体声端点尚未恢复；本次切换未通过验证。' }
            $result.WindowsStereoEndpoint=$stereo[0]
            $result.WindowsStereoVerified=$true
        } else { $result.StereoVerificationPending=$true }
    } else {
        $result.Readiness=Invoke-Native $deviceTool @('readiness') 'readiness'
        if (-not $result.Readiness.test_signing_active -or $result.Readiness.hvci_kernel_enforced) { throw 'The existing test driver loading conditions are not satisfied.' }
        # Stored rate/depth identify the encoder quality table. The
        # service follows live Windows PCM and validates the actual peer.
        $result.Profile=Invoke-Native $hostTool @('profile-check',"$SampleRate","$Bits","$Kbps") 'profile'
        $certificate=Join-Path $projectRoot 'build/test-certificate/lhdc-test.cer'
        $transportPackage=Join-Path $projectRoot 'build/test-package'
        $audioPackage=Join-Path $projectRoot 'build/test-audio-package'
        $result.TransportSignature=Invoke-Native $deviceTool @('verify-package',$transportPackage,$certificate) 'transport-signature'
        $result.AudioSignature=Invoke-Native $audioTool @('verify-package',$audioPackage,$certificate) 'audio-signature'
        $thumbprint=$result.AudioSignature.certificate_thumbprint
        if ($thumbprint -ne $result.TransportSignature.certificate_thumbprint) { throw 'Driver package certificates differ.' }
        $servicePath=Join-Path $env:ProgramFiles 'LHDC-Win/lhdc-service.exe'
        $installed=Get-LhdcService
        $deploymentFile=Join-Path $env:ProgramData 'LHDC-Win/deployment.json'
        $driversCurrent=$false
        $profileKey=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\LHDC-Win')
        $currentProfile=$null
        try { if ($null -ne $profileKey) { $currentProfile=$profileKey.GetValue('Profile') } }
        finally { if ($null -ne $profileKey) { $profileKey.Dispose() } }
        if ($null -ne $currentProfile -and ($currentProfile -isnot [byte[]] -or $currentProfile.Length -ne 12)) { throw 'Installed codec profile is invalid.' }
        $formatChanged=$null -eq $currentProfile -or $currentProfile.Length -ne 12 -or [BitConverter]::ToInt32($currentProfile,0) -ne $SampleRate -or [BitConverter]::ToInt32($currentProfile,4) -ne $Bits
        $hashes=[ordered]@{
            Transport=(Get-FileHash -LiteralPath (Join-Path $transportPackage 'lhdc-transport.sys') -Algorithm SHA256).Hash
            Audio=(Get-FileHash -LiteralPath (Join-Path $audioPackage 'lhdc-audio.sys') -Algorithm SHA256).Hash
            Service=(Get-FileHash -LiteralPath (Join-Path $projectRoot 'build/host/lhdc-service.exe') -Algorithm SHA256).Hash
        }
        if ($result.Before.service -eq 'lhdc-transport' -and $result.Before.problem -eq 0 -and $null -ne $installed -and $installed.State -eq 'Running' -and (Test-Path -LiteralPath $deploymentFile)) {
            $deployment=Get-Content -LiteralPath $deploymentFile -Encoding UTF8 | ConvertFrom-Json
            $driversCurrent=$deployment.Transport -eq $hashes.Transport -and $deployment.Audio -eq $hashes.Audio
            if ($driversCurrent -and $deployment.Service -eq $hashes.Service) {
                if ($installed.PathName -ne ('"'+$servicePath+'"') -or (Get-FileHash -LiteralPath $servicePath -Algorithm SHA256).Hash -ne $hashes.Service) { throw 'Installed service binary differs.' }
                if ($formatChanged) { $bound=$true; Stop-LhdcService }
                Write-CodecProfile
                if ($formatChanged) {
                    Restart-LhdcAudioChild
                    Wait-LhdcPlaybackFormat
                    Start-Service -Name 'LHDC-Win'
                    (Get-Service -Name 'LHDC-Win').WaitForStatus('Running',[TimeSpan]::FromSeconds(10))
                }
                $result.ProfileApplied=$true
                $result.Completed=$true
                return
            }
        }
        $bound=$result.Before.service -eq 'lhdc-transport'
        Stop-LhdcService
        Write-CodecProfile
        if ($bound -and -not $driversCurrent) {
            # The device installer deliberately accepts only the supported
            # Microsoft A2DP binding. Restore that binding before an update.
            $result.UpdateRestore=Restore-InboxAudio 'update-restore'
        }
        foreach ($store in @('Root','TrustedPublisher')) {
            if (-not (Test-Path -LiteralPath "Cert:/LocalMachine/$store/$thumbprint")) { Import-Certificate -FilePath $certificate -CertStoreLocation "Cert:/LocalMachine/$store" | Out-Null }
        }
        if (-not $driversCurrent) {
            $result.Stage=Invoke-Native $audioTool @('stage',(Join-Path $audioPackage 'lhdc-audio.inf'),$certificate) 'stage'
            $bound=$true
            $result.Binding=Invoke-Native $deviceTool @('install',$target,(Join-Path $transportPackage 'lhdc-transport.inf'),$certificate) 'bind'
        } else {
            $result.DriversRetained=$true
            if ($formatChanged) { Restart-LhdcAudioChild }
        }
        $deadline=(Get-Date).AddSeconds(30)
        do {
            $pcm=Invoke-Native $hostTool @('direct-pcm') 'pcm'
            if ($pcm.present) { break }
            [Threading.Thread]::Sleep(250)
        } while ((Get-Date) -lt $deadline)
        if (-not $pcm.present -or $pcm.abi -ne 1) { throw 'The headphone audio child and its PCM interface did not appear.' }
        $result.DirectPcm=$pcm
        $child=@(Get-PnpDevice -PresentOnly | Where-Object InstanceId -Like 'LHDCWIN\AUDIO\*')
        if ($child.Count -ne 1 -or $child[0].Status -ne 'OK') { throw 'The audio child is not healthy.' }
        $result.AudioChild=$child[0] | Select-Object InstanceId,Status,FriendlyName
        $parent=(Get-PnpDeviceProperty -InstanceId $child[0].InstanceId -KeyName DEVPKEY_Device_Parent).Data
        if ($parent -ne $target) { throw 'The audio child belongs to a different Bluetooth device.' }
        $result.AudioChildParent=$parent
        if (-not $driversCurrent) {
            $childInf=(Get-PnpDeviceProperty -InstanceId $child[0].InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data
            $childService=(Get-PnpDeviceProperty -InstanceId $child[0].InstanceId -KeyName DEVPKEY_Device_Service).Data
            if ($childInf -eq [IO.Path]::GetFileName($result.Stage.inf) -and $childService -eq 'lhdc-render') {
                # PnP normally binds the package staged before child enumeration.
                # Reinstalling that same binding can request an unnecessary reboot.
                $result.AudioBinding=[ordered]@{event='audio_child_binding';binding_verified=$true;reboot_required=$false;retained=$true;inf=$childInf}
            } else {
                $result.AudioBinding=Invoke-Native $audioTool @('install-child',$child[0].InstanceId,(Join-Path $audioPackage 'lhdc-audio.inf'),$certificate) 'audio-bind' -AllowRestart
                if ($result.AudioBinding.reboot_required) { throw '耳机音频驱动切换需要手动重启电脑，尚未完成。' }
            }
        }
        $result.DirectPcm=Invoke-Native $hostTool @('direct-pcm') 'pcm-after-bind'
        if (-not $result.DirectPcm.present) { throw 'Direct PCM interface disappeared after binding the audio driver.' }
        $result.ModeFormats=Invoke-Native $hostTool @('direct-formats') 'mode-formats'
        $installDirectory=Join-Path $env:ProgramFiles 'LHDC-Win'
        $dataDirectory=Join-Path $env:ProgramData 'LHDC-Win'
        foreach ($directory in @($installDirectory,$dataDirectory)) {
            if (Test-Path -LiteralPath $directory) {
                $item=Get-Item -LiteralPath $directory
                $owner=(Get-Acl -LiteralPath $directory).GetOwner([Security.Principal.SecurityIdentifier]).Value
                if (-not $item.PSIsContainer -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $owner -notin @('S-1-5-18','S-1-5-32-544')) { throw 'Service deployment directory is not owned by SYSTEM or Administrators, or is a reparse point.' }
            } else { New-Item -ItemType Directory -Path $directory | Out-Null }
        }
        # The LocalSystem service runs only from an administrator-writable path.
        $acl=[Security.AccessControl.DirectorySecurity]::new()
        $acl.SetOwner([Security.Principal.SecurityIdentifier]::new('S-1-5-32-544'))
        $acl.SetAccessRuleProtection($true,$false)
        foreach ($entry in @(@('S-1-5-18','FullControl'),@('S-1-5-32-544','FullControl'),@('S-1-5-32-545','ReadAndExecute'))) {
            $acl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new([Security.Principal.SecurityIdentifier]::new($entry[0]),$entry[1],'ContainerInherit,ObjectInherit','None','Allow'))
        }
        Set-Acl -LiteralPath $installDirectory -AclObject $acl
        Set-Acl -LiteralPath $dataDirectory -AclObject $acl
        foreach ($file in @($servicePath,(Join-Path $dataDirectory 'service.jsonl'))) {
            if (Test-Path -LiteralPath $file) {
                if ((Get-Item -LiteralPath $file).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'A service deployment file is a reparse point.' }
                Set-Acl -LiteralPath $file -AclObject $acl
            }
        }
        Copy-Item -LiteralPath (Join-Path $projectRoot 'build/host/lhdc-service.exe') -Destination $servicePath -Force
        $service=Get-LhdcService
        if ($null -eq $service) {
            New-Service -Name 'LHDC-Win' -DisplayName 'LHDC Bluetooth Audio' -BinaryPathName ('"'+$servicePath+'"') -StartupType Automatic -DependsOn 'AudioEndpointBuilder' | Out-Null
        } else {
            if ($service.PathName -ne ('"'+$servicePath+'"') -or $service.StartName -ne 'LocalSystem') { throw 'Existing LHDC service configuration differs.' }
            Set-Service -Name 'LHDC-Win' -StartupType Automatic
        }
        & sc.exe failure LHDC-Win reset= 86400 actions= restart/1000/restart/5000/restart/30000 | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'SCM recovery configuration failed.' }
        Wait-LhdcPlaybackFormat
        Start-Service -Name 'LHDC-Win'
        (Get-Service -Name 'LHDC-Win').WaitForStatus('Running',[TimeSpan]::FromSeconds(10))
        $result.After=Invoke-Native $deviceTool @('snapshot',$target) 'after'
        $result.Service=Get-LhdcService | Select-Object Name,State,StartMode,StartName,PathName
        if ($result.After.service -ne 'lhdc-transport' -or $result.After.problem -ne 0 -or $result.Service.State -ne 'Running') { throw 'Integrated audio installation did not verify.' }
        $hashes | ConvertTo-Json | Set-Content -LiteralPath $deploymentFile -Encoding UTF8
    }
    $result.Completed=$true
} catch {
    $result.Error=$_.Exception.Message
    $result.ErrorLocation=$_.ScriptStackTrace
    if ($bound) {
        try {
            Stop-LhdcService
            $result.Recovery=Restore-InboxAudio 'recovery'
            if ($null -ne $previousProfile) {
                $registry=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\LHDC-Win',$true)
                try {
                    $registry.SetValue('Profile',$previousProfile,[Microsoft.Win32.RegistryValueKind]::Binary)
                    if ($null -eq $previousAdaptive) { $registry.DeleteValue('AdaptiveBitrate',$false) }
                    else { $registry.SetValue('AdaptiveBitrate',$previousAdaptive,[Microsoft.Win32.RegistryValueKind]::DWord) }
                } finally { $registry.Dispose() }
            }
        } catch { $result.RecoveryError=$_.Exception.Message }
    }
} finally {
    $result.FinishedAt=Get-Date -Format o
    $json=$result | ConvertTo-Json -Depth 9
    $json | Set-Content -LiteralPath (Join-Path $evidence 'result.json') -Encoding UTF8
    if ($ResultFile) { $json | Set-Content -LiteralPath $ResultFile -Encoding UTF8 }
}
if (-not $result.Completed) { throw "Audio mode change failed: $($result.Error); recovery: $($result.RecoveryError). Evidence: $evidence" }
