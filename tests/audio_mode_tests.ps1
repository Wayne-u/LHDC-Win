$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=Split-Path -Parent $PSScriptRoot
$source=Get-Content -LiteralPath (Join-Path $root 'scripts/Set-AudioMode.ps1') -Raw -Encoding UTF8
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseInput($source,[ref]$tokens,[ref]$errors)
if ($errors.Count) { throw ($errors.Message -join '; ') }
$function=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Restore-InboxAudio'},$true)
if ($null -eq $function) { throw 'Inbox restoration function not found.' }
# Exercise the production orchestration with fake PnP/native operations only.
. ([scriptblock]::Create($function.Extent.Text))
$target='headphones';$deviceTool='device';$hostTool='host'
$events=[Collections.Generic.List[string]]::new()
function Get-LhdcService { [pscustomobject]@{State='Stopped'} }
function Set-Service { param($Name,$StartupType) $events.Add('disable-service') }
function Get-PnpDevice { param([switch]$PresentOnly) if ($script:childPresent) { [pscustomobject]@{InstanceId='LHDCWIN\AUDIO\Audio0'} } }
function Get-PnpDeviceProperty { param($InstanceId,$KeyName) [pscustomobject]@{Data='different-headphones'} }
function Set-AvrcpOwnership { param([bool]$Own) if ($Own) { throw 'Restore must release AVRCP.' };$events.Add('release-avrcp') }
function Invoke-Native {
    param($Tool,$Arguments,$Name,[switch]$AllowRestart)
    switch ($Arguments[0]) {
        'snapshot' { [pscustomobject]@{service=$script:binding} }
        'inspect' { [pscustomobject]@{transport_interfaces=if ($script:pendingBinding) { @() } else { @('transport') }} }
        'audio-detach' {
            $events.Add('detach')
            if ($script:detachFails) { throw 'Detach failed.' }
            if (-not $script:foreignChild) { $script:childPresent=$false }
        }
        'restore' {
            if ($script:childPresent) { throw 'Driver binding attempted while audio clients still own the child.' }
            $events.Add('bind-inbox')
            [pscustomobject]@{reboot_required=$script:restart;binding_verified=$true}
        }
        default { throw 'Unexpected native operation.' }
    }
}
function Invoke-DriverChange { param($Arguments,$Name) Invoke-Native $deviceTool $Arguments $Name -AllowRestart }
function Require([bool]$Condition,[string]$Message) { if (-not $Condition) { throw $Message } }
foreach ($case in @('connected','already-inbox','pending-binding','detach-error','foreign-child','restart')) {
    $result=[ordered]@{};$bound=$true;$events.Clear()
    $binding=if ($case -eq 'already-inbox') { 'BthA2dp' } else { 'lhdc-transport' }
    $pendingBinding=$case -eq 'pending-binding'
    $childPresent=$binding -eq 'lhdc-transport' -and -not $pendingBinding
    $detachFails=$case -eq 'detach-error';$foreignChild=$case -eq 'foreign-child';$restart=$case -eq 'restart'
    $failure=$null
    try { $null=Restore-InboxAudio 'test' } catch { $failure=$_.Exception.Message }
    if ($case -in @('connected','already-inbox','pending-binding')) {
        Require ($null -eq $failure -and -not $bound) "$case did not restore the binding."
        $expected=if ($case -eq 'connected') { 'disable-service,detach,bind-inbox,release-avrcp' } else { 'disable-service,bind-inbox,release-avrcp' }
        Require (($events -join ',') -eq $expected) 'Audio clients must detach before binding, and AVRCP must restore after it.'
    } elseif ($case -eq 'restart') {
        Require ($failure -match '重启' -and -not $bound -and $events.Contains('release-avrcp')) 'A pending reboot must remain an explicit failure and restore native AVRCP.'
    } else {
        Require ($null -ne $failure -and $bound -and -not $events.Contains('bind-inbox')) 'A failed detach or foreign child must prevent binding.'
    }
}
$driverChange=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Invoke-DriverChange'},$true)
if ($null -eq $driverChange) { throw 'Driver change function not found.' }
. ([scriptblock]::Create($driverChange.Extent.Text))
function Get-Service {
    param($Name)
    $children=if ($Name -eq 'Audiosrv') { @([pscustomobject]@{Name='AudioVendor'},[pscustomobject]@{Name='StoppedDependent'}) } else { @() }
    $service=[pscustomobject]@{Name=$Name;Status=$script:states[$Name];DependentServices=$children}
    $service | Add-Member -MemberType ScriptMethod -Name WaitForStatus -Value { param($expected,$timeout) if ($script:states[$this.Name] -ne $expected) { throw 'Unexpected service status.' } }
    return $service
}
function Stop-Service {
    param($Name)
    if ($Name -eq 'Audiosrv' -and $script:states.AudioVendor -eq 'Running') { throw 'Audio dependency must stop first.' }
    $events.Add('stop-'+$Name);$script:states[$Name]='Stopped'
}
function Start-Service { param($Name) $events.Add('start-'+$Name);$script:states[$Name]='Running' }
function Invoke-Native {
    param($Tool,$Arguments,$Name,[switch]$AllowRestart)
    Require ($script:states.Audiosrv -eq 'Stopped' -and $script:states.AudioVendor -eq 'Stopped') 'Driver binding must release audio engine handles first.'
    Require $AllowRestart 'Binding restart result must be interpreted explicitly.'
    $events.Add('bind')
    if ($script:bindingFails) { throw 'Simulated driver install failure.' }
    [pscustomobject]@{reboot_required=$script:restart;binding_verified=$true}
}
foreach($case in @('success','native-error','restart','already-stopped')) {
    $events.Clear();$bindingFails=$case -eq 'native-error';$restart=$case -eq 'restart'
    $state=if ($case -eq 'already-stopped') { 'Stopped' } else { 'Running' }
    $states=@{Audiosrv=$state;AudioVendor=$state;StoppedDependent='Stopped'}
    $failure=$null;$record=$null
    try { $record=Invoke-DriverChange @('install',$target) 'test' } catch { $failure=$_.Exception.Message }
    Require ($states.Audiosrv -eq $state -and $states.AudioVendor -eq $state -and $states.StoppedDependent -eq 'Stopped') 'Driver change must restore prior service states and leave stopped dependencies stopped.'
    $expected=if ($case -eq 'already-stopped') { 'bind' } else { 'stop-AudioVendor,stop-Audiosrv,bind,start-Audiosrv,start-AudioVendor' }
    Require (($events -join ',') -eq $expected) 'Audio service lifecycle ordering differs.'
    if ($bindingFails) { Require ($failure -eq 'Simulated driver install failure.') 'The install error must survive audio service restoration.' }
    else { Require ($null -eq $failure -and $record.reboot_required -eq $restart) 'Binding and restart status must be returned to the caller.' }
}
Write-Host 'Inbox restoration, pending bindings, audio engine release, service restoration, errors and reboot checks passed; no devices changed.'
