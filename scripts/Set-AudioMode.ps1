param(
    [Parameter(Mandatory)][string]$PackageRoot,
    [ValidateSet('LHDC','Windows')][string]$Mode='LHDC',
    [ValidateSet(44100,48000,96000,192000)][int]$SampleRate=48000,
    [ValidateSet(16,24)][int]$Bits=24,
    [int]$Kbps=400,
    [switch]$AdaptiveBitrate,
    [string]$DeviceInstance,
    [string]$ResultFile
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$OutputEncoding=[Console]::OutputEncoding
$PackageRoot=[IO.Path]::GetFullPath($PackageRoot)
$evidence=Join-Path $env:LOCALAPPDATA ('LHDC-Win/deployment/integrated-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff')+'-'+$Mode)
New-Item -ItemType Directory -Path $evidence -Force | Out-Null
$result=[ordered]@{Mode=$Mode;StartedAt=Get-Date -Format o;EvidenceDirectory=$evidence;Completed=$false;Error=$null;RecoveryError=$null}
$deviceTool=Join-Path $PackageRoot 'backend/lhdc-device.exe'
$audioTool=Join-Path $PackageRoot 'backend/lhdc-audio-device.exe'
$hostTool=Join-Path $PackageRoot 'backend/lhdc-host.exe'
$target=''
function Invoke-Native([string]$Tool,[string[]]$Arguments,[string]$Name,[switch]$AllowRestart) {
    if ($Tool -eq $hostTool -and $target) { $Arguments=@('--device',$target)+$Arguments }
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
function Invoke-DriverChange([string[]]$Arguments,[string]$Name) {
    $stopped=[Collections.Generic.List[string]]::new()
    function Stop-AudioTree([string]$ServiceName) {
        $service=Get-Service -Name $ServiceName
        if ($service.Status -ne 'Running') { return }
        foreach($dependent in $service.DependentServices) { Stop-AudioTree $dependent.Name }
        Stop-Service -Name $ServiceName
        $stopped.Add($ServiceName)
        (Get-Service -Name $ServiceName).WaitForStatus('Stopped',[TimeSpan]::FromSeconds(15))
    }
    try {
        # Both Endpoint Builder and audiodg can retain the old KS filters and
        # veto device removal. Release their handles before replacing the stack.
        Stop-AudioTree 'AudioEndpointBuilder'
        return Invoke-Native $deviceTool $Arguments $Name -AllowRestart
    } finally {
        for($i=$stopped.Count-1;$i -ge 0;$i--) {
            Start-Service -Name $stopped[$i]
            (Get-Service -Name $stopped[$i]).WaitForStatus('Running',[TimeSpan]::FromSeconds(15))
        }
    }
}
function Restore-InboxAudio([string]$Name) {
    if ($null -ne (Get-LhdcService)) { Set-Service -Name 'LHDC-Win' -StartupType Disabled }
    $current=Invoke-Native $deviceTool @('snapshot',$target) ($Name+'-before')
    if ($current.service -eq 'lhdc-transport') {
        $inventory=Invoke-Native $hostTool @('inspect') ($Name+'-interfaces')
        # A pending driver change updates Service/INF before the new stack is
        # loaded. Its private transport interface does not exist yet.
        if (@($inventory.transport_interfaces).Count -gt 0) {
            $null=Invoke-Native $hostTool @('audio-detach') ($Name+'-detach')
        }
        $deadline=(Get-Date).AddSeconds(10)
        do {
            $children=@(Get-PnpDevice -PresentOnly | Where-Object InstanceId -Like 'LHDCWIN\AUDIO\*')
            foreach($child in $children) {
                $parent=(Get-PnpDeviceProperty -InstanceId $child.InstanceId -KeyName DEVPKEY_Device_Parent).Data
                if ($parent -ne $target) { throw 'The LHDC audio child belongs to a different device.' }
            }
            if ($children.Count -eq 0) { break }
            [Threading.Thread]::Sleep(250)
        } while ((Get-Date) -lt $deadline)
        if ($children.Count) { throw 'LHDC 音频设备仍在释放中，尚未切换 Windows 驱动。请稍后重试。' }
    }
    $record=Invoke-DriverChange @('restore',$target) $Name
    $script:bound=$false
    $result.RebootRequired=[bool]$record.reboot_required
    Restore-NativeAvrcp
    if ($record.reboot_required) { throw 'Windows 驱动已绑定，但设备仍需重启才能完成切换。请手动重启电脑；尚未确认立体声恢复。' }
    return $record
}
function Restore-NativeAvrcp {
    $null=Set-NativeAvrcpOwnership $false
}
function Save-NativeAvrcpIdentity([string]$Instance,[bool]$Own) {
    $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\LHDC-Win\Device',$true)
    try {
        if ($Own) { $key.SetValue('NativeAvrcpInstance',$Instance,[Microsoft.Win32.RegistryValueKind]::String) }
        else { $key.DeleteValue('NativeAvrcpInstance',$false) }
    } finally { $key.Dispose() }
}
function Set-NativeAvrcpOwnership([bool]$Own) {
    $stateFile=Join-Path $env:ProgramData 'LHDC-Win/native-avrcp.json'
    if (-not $Own -and -not (Test-Path -LiteralPath $stateFile)) { return $false }
    $address=$device.address.Replace(':','')
    $controllers=@(Get-PnpDevice -PresentOnly | Where-Object {
        $_.InstanceId -match '^BTHENUM\\\{0000110E-0000-1000-8000-00805F9B34FB\}' -and
        $_.InstanceId -like ('*&'+$address+'_*')
    })
    if ($Own -and $controllers.Count -eq 0) { return $true }
    if ($Own -and $controllers.Count -gt 1 -and -not (Test-Path -LiteralPath $stateFile)) { return $false }
    if ($controllers.Count -ne 1) { throw 'Expected one native AVRCP controller for the target headphones.' }
    $instance=$controllers[0].InstanceId
    $service=(Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_Service).Data
    if ($service -ne 'Microsoft_Bluetooth_AvrcpTransport') {
        if ($Own -and -not (Test-Path -LiteralPath $stateFile)) { return $false }
        throw 'The target AVRCP controller has an unrelated driver.'
    }
    $problem=(Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_ProblemCode).Data
    if ($problem -notin @(0,22)) { throw "The native AVRCP controller has a device problem: $problem." }
    if (Test-Path -LiteralPath $stateFile) {
        $state=Get-Content -LiteralPath $stateFile -Encoding UTF8 | ConvertFrom-Json
    } else {
        $state=[pscustomobject]@{Instance=$instance;WasEnabled=($problem -eq 0)}
        $state | ConvertTo-Json | Set-Content -LiteralPath $stateFile -Encoding UTF8
        Set-Acl -LiteralPath $stateFile -AclObject (Get-Acl -LiteralPath (Split-Path -Parent $stateFile))
    }
    if ($state.Instance -ne $instance -or $state.WasEnabled -isnot [bool]) { throw 'Saved AVRCP ownership does not match the target headphones.' }
    if ($Own -and $problem -eq 0) { Disable-PnpDevice -InstanceId $instance -Confirm:$false }
    elseif (-not $Own -and $state.WasEnabled -and $problem -eq 22) { Enable-PnpDevice -InstanceId $instance -Confirm:$false }
    $after=(Get-PnpDeviceProperty -InstanceId $instance -KeyName DEVPKEY_Device_ProblemCode).Data
    if (($Own -and $after -ne 22) -or (-not $Own -and $state.WasEnabled -and $after -ne 0)) { throw 'Native AVRCP ownership did not verify.' }
    Save-NativeAvrcpIdentity $instance $Own
    if (-not $Own) { Remove-Item -LiteralPath $stateFile }
    $result.NativeAvrcpOwnership=[ordered]@{Instance=$instance;LhdcOwnsController=$Own;OriginallyEnabled=$state.WasEnabled}
    return $true
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
    # The next dynamically enumerated child reads the new format itself.
    if ($children.Count -eq 0) { return }
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
    # The topology starts unplugged. Publish the ACL state before Endpoint
    # Builder readiness checks, including when installing with the earbuds off.
    $null=Invoke-Native $hostTool @('audio-connection') 'audio-connection'
    if (-not $connection.connected) { $result.FormatVerificationPending=$true; return }
    $inputBits=$Bits
    $deadline=(Get-Date).AddSeconds(20)
    $stableId=$null
    $stableSince=$null
    $stabilityWindow=[TimeSpan]::FromSeconds(2)
    $container=([guid](Get-PnpDeviceProperty -InstanceId $target -KeyName DEVPKEY_Device_ContainerId).Data).ToString('B')
    $volumeKey=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\LHDC-Win\Device')
    try { $expectedVolume=if ($volumeKey.GetValue('AbsoluteVolume',0) -eq 1) { 3 } else { 0 } }
    finally { $volumeKey.Dispose() }
    do {
        $devices=Invoke-Native $hostTool @('audio-devices') 'format-endpoints'
        $endpoint=@($devices.devices | Where-Object { $_.adapter_id -like '*lhdcwin#audio#*' -and $_.container_id -eq $container -and $_.channels -eq 2 -and $_.sample_rate -eq $SampleRate -and $_.device_sample_rate -eq $SampleRate -and $_.device_bits -eq $inputBits -and ($_.volume_hardware_support -band 3) -eq $expectedVolume })
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
function Initialize-VolumeMemory([bool]$Absolute,$Previous,$Probe) {
    $key=[Microsoft.Win32.Registry]::LocalMachine.CreateSubKey('SOFTWARE\LHDC-Win\Volume\'+$device.address.Replace(':','').ToUpperInvariant())
    try {
        $saved=$key.GetValue('State')
        if ($null -ne $saved) {
            if ($saved -isnot [byte[]] -or $saved.Length -ne 16 -or [BitConverter]::ToUInt32($saved,0) -ne 1 -or
                -not [float]::IsFinite([BitConverter]::ToSingle($saved,4)) -or [BitConverter]::ToSingle($saved,4) -lt 0 -or [BitConverter]::ToSingle($saved,4) -gt 1 -or
                [BitConverter]::ToUInt32($saved,8) -gt 1 -or [BitConverter]::ToUInt32($saved,12) -gt 1) { throw 'Saved headphone volume is invalid.' }
            if ([BitConverter]::ToUInt32($saved,12) -eq [uint32]$Absolute) { return }
        }
        if ($null -eq $Previous) { return }
        [float]$scalar=$Previous.volume_scalar
        if ($Absolute) { $scalar=[Math]::Min($scalar,[float]($Probe.remote_volume/127.0)) }
        [byte[]]$state=@([BitConverter]::GetBytes([uint32]1)+[BitConverter]::GetBytes($scalar)+[BitConverter]::GetBytes([uint32][bool]$Previous.muted)+[BitConverter]::GetBytes([uint32]$Absolute))
        $key.SetValue('State',$state,[Microsoft.Win32.RegistryValueKind]::Binary)
        $result.VolumeMemoryInitialized=[ordered]@{Scalar=$scalar;Muted=[bool]$Previous.muted;Absolute=$Absolute}
    } finally { $key.Dispose() }
}
$bound=$false
$previousProfile=$null
$previousAdaptive=$null
try {
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    if (-not ([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Audio mode installation requires an administrator process.' }
    $discoveryArguments=@('target')
    if ($DeviceInstance) { $discoveryArguments=@('--device',$DeviceInstance)+$discoveryArguments }
    $deviceText=@(& $hostTool @discoveryArguments)
    if ($LASTEXITCODE -ne 0) { throw 'Target device discovery failed.' }
    $device=$deviceText[-1] | ConvertFrom-Json
    if (-not $device.found) { throw '请先配对并在面板选择一台蓝牙耳机。' }
    $target=$device.instance
    $result.Device=$device
    $allTargets=Invoke-Native $hostTool @('targets') 'targets'
    if (@($allTargets.devices | Where-Object { $_.service -eq 'lhdc-transport' -and $_.instance -ne $target }).Count) {
        throw '请先把当前使用 LHDC 的耳机恢复为 Windows 音频，再切换到另一台耳机。'
    }
    $result.Before=Invoke-Native $deviceTool @('snapshot',$target) 'before'
    if ($result.Before.service -notin @('BthA2dp','lhdc-transport')) {
        throw "所选耳机当前使用其他音频驱动（$($result.Before.service)）。请先通过原驱动工具或设备管理器将这台耳机恢复为微软蓝牙 A2DP 驱动，再重试。"
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
            $container=([guid](Get-PnpDeviceProperty -InstanceId $target -KeyName DEVPKEY_Device_ContainerId).Data).ToString('B')
            $stereoReady=$false
            do {
                $devices=Invoke-Native $hostTool @('audio-devices') 'windows-endpoints'
                $stereo=@($devices.devices | Where-Object { $_.container_id -eq $container -and $_.adapter_id -notlike '*lhdcwin#audio#*' -and $_.channels -eq 2 -and $_.device_sample_rate -ge 44100 })
                if ($stereo.Count -eq 1) {
                    $ready=Invoke-Native $hostTool @('playback-ready',$stereo[0].id,"$($stereo[0].device_sample_rate)",'16') 'windows-readiness'
                    if ($ready.ready) { $stereoReady=$true; break }
                }
                [Threading.Thread]::Sleep(250)
            } while ((Get-Date) -lt $deadline)
            if (-not $stereoReady) { throw 'Windows 原生驱动已绑定，但双声道立体声端点尚未恢复；本次切换未通过验证。' }
            $result.WindowsStereoEndpoint=$stereo[0]
            $result.WindowsStereoVerified=$true
        } else { $result.StereoVerificationPending=$true }
    } else {
        if (-not $device.connected) { throw '首次安装或更新 LHDC 播放驱动前，请连接所选耳机，以便验证真实的 LHDC V5 能力。' }
        $os=Get-CimInstance -ClassName Win32_OperatingSystem
        $result.OperatingSystem=[ordered]@{BuildNumber=[int]$os.BuildNumber;Architecture=[Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()}
        if ($os.ProductType -ne 1 -or [int]$os.BuildNumber -lt 22621 -or $result.OperatingSystem.Architecture -ne 'X64') {
            throw 'LHDC requires Windows 11 22H2 or later (build 22621+), running on x64 hardware.'
        }
        $result.Readiness=Invoke-Native $deviceTool @('readiness') 'readiness'
        if (-not $result.Readiness.test_signing_active) {
            throw '当前系统的测试签名模式尚未生效。请按项目说明启用测试模式并完成重启，再重试安装。'
        }
        if ($result.Readiness.hvci_kernel_enforced) {
            throw '当前系统正在强制执行内存完整性（HVCI），不满足本测试签名驱动的加载条件。请按项目说明检查驱动加载配置后重试。'
        }
        # Stored rate/depth identify the encoder quality table. The
        # service follows live Windows PCM and validates the actual peer.
        $result.Profile=Invoke-Native $hostTool @('profile-check',"$SampleRate","$Bits","$Kbps") 'profile'
        $certificate=Join-Path $PackageRoot 'drivers/lhdc-test.cer'
        $transportPackage=Join-Path $PackageRoot 'drivers/transport'
        $audioPackage=Join-Path $PackageRoot 'drivers/audio'
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
            Service=(Get-FileHash -LiteralPath (Join-Path $PackageRoot 'backend/lhdc-service.exe') -Algorithm SHA256).Hash
        }
        $volumeBefore=Invoke-Native $hostTool @('audio-devices') 'volume-before'
        $previousVolume=@($volumeBefore.devices | Where-Object { $_.container_id -eq $device.container_id -and $_.channels -eq 2 -and
            (($_.adapter_id -like '*lhdcwin#audio#*') -eq ($result.Before.service -eq 'lhdc-transport')) })
        if ($previousVolume.Count -eq 1) { $previousVolume=$previousVolume[0] } else { $previousVolume=$null }
        if ($result.Before.service -eq 'lhdc-transport' -and $result.Before.problem -eq 0 -and $null -ne $installed -and (Test-Path -LiteralPath $deploymentFile)) {
            $deployment=Get-Content -LiteralPath $deploymentFile -Encoding UTF8 | ConvertFrom-Json
            $driversCurrent=$deployment.Transport -eq $hashes.Transport -and $deployment.Audio -eq $hashes.Audio
        }
        $bound=$result.Before.service -eq 'lhdc-transport'
        Stop-LhdcService
        $result.Selection=Invoke-Native $deviceTool @('select',$target) 'select-device'
        $volumeKey=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\LHDC-Win\Device')
        try { $previousVolumeMode=[int]$volumeKey.GetValue('AbsoluteVolume',0) }
        finally { $volumeKey.Dispose() }
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
            $result.Binding=Invoke-DriverChange @('install',$target,(Join-Path $transportPackage 'lhdc-transport.inf'),$certificate) 'bind'
            if ($result.Binding.reboot_required) { throw 'Windows 尚未卸载旧音频驱动，LHDC 驱动等待系统重启，切换尚未完成。' }
        } else {
            $result.DriversRetained=$true
        }
        # Release any old controller before probing the selected peer anew.
        Restore-NativeAvrcp
        $result.Compatibility=Invoke-Native $hostTool @('probe',"$SampleRate","$Bits","$Kbps") 'playback-compatibility'
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
        $absolute=$false
        # Probe remote volume support without changing the earbud's gain.
        # Only supported peers use AVRCP; others retain software PCM volume.
        if (Set-NativeAvrcpOwnership $true) {
            $result.VolumeProbe=Invoke-Native $hostTool @('volume-probe') 'volume-probe'
            $absolute=[bool]$result.VolumeProbe.supported
        }
        if (-not $absolute) { Restore-NativeAvrcp }
        Initialize-VolumeMemory $absolute $previousVolume $(if ($absolute) { $result.VolumeProbe } else { $null })
        $volumeKey=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\LHDC-Win\Device',$true)
        try { $volumeKey.SetValue('AbsoluteVolume',[int]$absolute,[Microsoft.Win32.RegistryValueKind]::DWord) }
        finally { $volumeKey.Dispose() }
        $result.AbsoluteVolume=$absolute
        if ($formatChanged -or $previousVolumeMode -ne [int]$absolute) { Restart-LhdcAudioChild }
        $registry=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SOFTWARE\LHDC-Win',$true)
        try {
            $registry.SetValue('PeerCapabilities',[Convert]::FromHexString($result.Compatibility.peer_capabilities),[Microsoft.Win32.RegistryValueKind]::Binary)
            $registry.SetValue('PeerDeviceInstance',$target,[Microsoft.Win32.RegistryValueKind]::String)
            $registry.SetValue('PeerMediaMtu',[int]$result.Compatibility.media_mtu,[Microsoft.Win32.RegistryValueKind]::DWord)
            $revision=$registry.GetValue('CapabilityRevision',0)
            $registry.SetValue('PeerCapabilityRevision',[int]$revision,[Microsoft.Win32.RegistryValueKind]::DWord)
        } finally { $registry.Dispose() }
        # A disconnected earbud has no audio child. Ask the transport to
        # enumerate it before checking PCM, and defer those checks when off.
        $audioConnection=Invoke-Native $hostTool @('audio-connection') 'audio-presence'
        if ($audioConnection.connected) {
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
        } else { $result.FormatVerificationPending=$true }
        Copy-Item -LiteralPath (Join-Path $PackageRoot 'backend/lhdc-service.exe') -Destination $servicePath -Force
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
