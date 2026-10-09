$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$root=Split-Path -Parent $PSScriptRoot
$source=Get-Content -LiteralPath (Join-Path $root 'scripts/Verify-Playback.ps1') -Encoding UTF8 -Raw
$tokens=$null;$errors=$null
$ast=[Management.Automation.Language.Parser]::ParseInput($source,[ref]$tokens,[ref]$errors)
if($errors.Count) { throw ($errors.Message -join '; ') }
$function=$ast.Find({param($node) $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq 'Get-LongPlaybackFailures'},$true)
if($null -eq $function) { throw 'Playback assessment function not found.' }
# Load only the pure assessment function; never start playback or change devices.
. ([scriptblock]::Create($function.Extent.Text))
function New-Session {
    [pscustomobject]@{packets=5891;bytes=3051538;completed_bytes=3051538;dropped_bytes_total=0;max_send_us=329618;pending_signal=0;pending_media=0;pending_avrcp=0}
}
function Require([bool]$Condition,[string]$Message) {
    if(-not $Condition) { throw $Message }
}
$session=New-Session
Require (@(Get-LongPlaybackFailures $true @($session) @() '').Count -eq 0) 'Lossless completed playback must pass.'
$session.dropped_bytes_total=40320
$reasons=@(Get-LongPlaybackFailures $true @($session) @() '')
Require ($reasons.Count -eq 1 -and $reasons[0] -match '40320' -and $reasons[0] -match '329618') 'Completed playback with PCM loss must explain the failure.'
$session=New-Session
$session.completed_bytes--
Require (@(Get-LongPlaybackFailures $true @($session) @() '')[0] -match 'completion mismatch') 'Incomplete media writes must fail.'
foreach($channel in @('pending_signal','pending_media','pending_avrcp')) {
    $session=New-Session;$session.$channel=1
    Require (@(Get-LongPlaybackFailures $true @($session) @() '')[0] -match 'Pending Bluetooth') "Pending channel must fail: $channel"
}
Require (@(Get-LongPlaybackFailures $true @() @() '')[0] -match 'found 0') 'A missing session must fail.'
Require (@(Get-LongPlaybackFailures $true @((New-Session),(New-Session)) @() '')[0] -match 'found 2') 'Multiple sessions must fail.'
$session=New-Session;$session.packets=0
Require (@(Get-LongPlaybackFailures $true @($session) @() '')[0] -match 'no media packets') 'A zero-packet session must fail.'
Require (@(Get-LongPlaybackFailures $true @((New-Session)) @([pscustomobject]@{event='service_audio_error'}) '')[0] -match 'Service audio errors') 'Service errors must fail.'
Require (@(Get-LongPlaybackFailures $true @((New-Session)) @() 'Endpoint query failed.')[0] -eq 'Endpoint query failed.') 'Runtime errors must not turn into a pass after render completion.'
Require (@(Get-LongPlaybackFailures $false @((New-Session)) @() '')[0] -match 'did not complete') 'An incomplete duration must fail.'
Write-Host 'Long playback assessment checks passed; no audio or device operations performed.'
