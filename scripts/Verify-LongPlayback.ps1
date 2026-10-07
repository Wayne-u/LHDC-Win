param([ValidateRange(1,120)][int]$Minutes=30,[switch]$Audible,[string]$ResultFile)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$OutputEncoding=[Console]::OutputEncoding
$root=Split-Path -Parent $PSScriptRoot
$tool=Join-Path $root 'build/host/lhdc-host.exe'
$log=Join-Path $env:ProgramData 'LHDC-Win/service.jsonl'
$dir=Join-Path $root ('evidence/local/long-playback-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $dir|Out-Null
$result=[ordered]@{StartedAt=Get-Date -Format o;Minutes=$Minutes;Completed=$false;Passed=$false;Error=$null;EvidenceDirectory=$dir;HearingVerified=$false;Silent=(-not $Audible)}
$render=$null
$offset=$null
$sessions=@()
$audioErrors=@()
function Native([string[]]$Arguments) {
    $text=@(& $tool @Arguments)
    if($LASTEXITCODE) { throw "Backend query failed: $($Arguments[0])." }
    $text[-1]|ConvertFrom-Json
}
function Get-LongPlaybackFailures([bool]$Completed,[object[]]$Sessions,[object[]]$AudioErrors,[string]$RuntimeError) {
    if($RuntimeError) { $RuntimeError }
    elseif(-not $Completed) { 'Requested playback duration did not complete.' }
    if($Sessions.Count -ne 1) { "Expected one completed LHDC session; found $($Sessions.Count)." }
    foreach($session in $Sessions) {
        if($session.packets -le 0) { 'LHDC session sent no media packets.' }
        if($session.dropped_bytes_total -ne 0) {
            "PCM dropped $($session.dropped_bytes_total) bytes; maximum media send time $($session.max_send_us) us."
        }
        if($session.completed_bytes -ne $session.bytes) {
            "Media completion mismatch: submitted $($session.bytes) bytes, completed $($session.completed_bytes) bytes."
        }
        if($session.pending_signal -ne 0 -or $session.pending_media -ne 0 -or $session.pending_avrcp -ne 0) {
            "Pending Bluetooth operations: signal=$($session.pending_signal), media=$($session.pending_media), avrcp=$($session.pending_avrcp)."
        }
    }
    if($AudioErrors.Count) { "Service audio errors: $($AudioErrors.Count); see service.jsonl." }
}
try {
    $status=Native @('audio-status')
    if(-not $status.running) { throw 'LHDC service is not running.' }
    $result.ServiceHash=(Get-FileHash -LiteralPath (Join-Path $env:ProgramFiles 'LHDC-Win/lhdc-service.exe') -Algorithm SHA256).Hash
    if($result.ServiceHash -ne (Get-FileHash -LiteralPath (Join-Path $root 'build/host/lhdc-service.exe') -Algorithm SHA256).Hash) {
        throw 'Install the completed service build before collecting long playback metrics.'
    }
    $profile=(Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/LHDC-Win').Profile
    $rate=[BitConverter]::ToUInt32($profile,0);$bits=[BitConverter]::ToUInt32($profile,4)
    $devices=Native @('audio-devices')
    $source=@($devices.devices|Where-Object adapter_id -Like '*lhdcwin#audio#*')
    if($source.Count -ne 1 -or $source[0].device_sample_rate -ne $rate -or $source[0].device_bits -ne $bits) { throw 'Selected LHDC playback endpoint is unavailable.' }
    $result.Before=$devices
    $wav=Join-Path $dir 'source.wav'
    if($Audible) { $null=Native @('make-test-wav',$wav,'10',"$rate","$bits",'--loop') }
    else {
        $size=10*$rate*2*($bits/8)
        $writer=[IO.BinaryWriter]::new([IO.File]::Create($wav),[Text.Encoding]::UTF8)
        try {
            $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'));$writer.Write([uint32](36+$size))
            $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '));$writer.Write([uint32]16)
            $writer.Write([uint16]1);$writer.Write([uint16]2);$writer.Write([uint32]$rate)
            $writer.Write([uint32]($rate*2*($bits/8)));$writer.Write([uint16](2*($bits/8)));$writer.Write([uint16]$bits)
            $writer.Write([Text.Encoding]::ASCII.GetBytes('data'));$writer.Write([uint32]$size);$writer.Write([byte[]]::new($size))
        } finally { $writer.Dispose() }
    }
    $offset=(Get-Item -LiteralPath $log).Length
    $render=Start-Process $tool -ArgumentList 'render-wav',$source[0].id,('"'+$wav+'"'),"$($Minutes*6)" -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $dir 'render.jsonl') -RedirectStandardError (Join-Path $dir 'render.err')
    $deadline=(Get-Date).AddMinutes($Minutes).AddSeconds(20)
    while(-not $render.WaitForExit(10000)) {
        $pcm=Native @('direct-pcm')
        @{Time=Get-Date -Format o;PCM=$pcm}|ConvertTo-Json -Depth 4 -Compress|Add-Content -LiteralPath (Join-Path $dir 'samples.jsonl') -Encoding UTF8
        if(-not $pcm.running -or $pcm.sample_rate -ne $rate -or $pcm.bits -ne $bits) { throw 'Continuous PCM playback stopped or changed format.' }
        if((Get-Date) -gt $deadline) { throw 'Long playback exceeded its deadline.' }
    }
    if($render.ExitCode) { throw 'Long playback renderer failed; see render.err.' }
    $result.Render=@(Get-Content -LiteralPath (Join-Path $dir 'render.jsonl') -Encoding UTF8|ForEach-Object { $_|ConvertFrom-Json })
    $completion=@($result.Render|Where-Object event -EQ 'wav_render_complete')
    if($completion.Count -ne 1 -or $completion[0].frames -ne ($Minutes*60*$rate)) {
        throw 'The continuous renderer did not submit the full requested duration.'
    }
    Start-Sleep -Seconds 2
    $result.After=Native @('audio-devices')
    $result.Completed=$true
} catch { $result.Error=$_.Exception.Message }
finally {
    if($null -ne $render -and -not $render.HasExited) { $render.Kill();$render.WaitForExit() }
    $sourceWav=Join-Path $dir 'source.wav'
    if(Test-Path -LiteralPath $sourceWav) { Remove-Item -LiteralPath $sourceWav }
    if($null -ne $offset) {
        $file=[IO.FileStream]::new($log,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
        try { $null=$file.Seek($offset,[IO.SeekOrigin]::Begin);$reader=[IO.StreamReader]::new($file,[Text.Encoding]::UTF8);$text=$reader.ReadToEnd() }
        finally { $file.Dispose() }
        $text|Set-Content -LiteralPath (Join-Path $dir 'service.jsonl') -Encoding UTF8
        $events=@($text -split "`n"|Where-Object { $_.Trim() }|ForEach-Object {$_|ConvertFrom-Json})
        $sessions=@($events|Where-Object event -EQ 'direct_audio_complete')
        $result.Sessions=$sessions
        $audioErrors=@($events|Where-Object event -In @('service_audio_error','service_fatal_error','signalling_monitor_failed'))
        $result.AudioErrors=$audioErrors
        $result.SendStalls=@($events|Where-Object event -EQ 'media_send_stall')
        $progress=@($events|Where-Object event -EQ 'direct_audio_progress')
        if($progress.Count) {
            $result.MaxSendUs=($progress|Measure-Object max_send_us -Maximum).Maximum
            $result.MaxReadGapUs=($progress|Measure-Object max_read_gap_us -Maximum).Maximum
            if($progress[0].PSObject.Properties.Name -contains 'max_hfp_check_us') {
                $result.MaxHfpCheckUs=($progress|Measure-Object max_hfp_check_us -Maximum).Maximum
            }
        }
    }
    $result.FailureReasons=@(Get-LongPlaybackFailures -Completed $result.Completed -Sessions $sessions -AudioErrors $audioErrors -RuntimeError $result.Error)
    $result.Passed=$result.FailureReasons.Count -eq 0
    if(-not $result.Passed) { $result.Error=$result.FailureReasons -join ' ' }
    $result.FinishedAt=Get-Date -Format o
    $json=$result|ConvertTo-Json -Depth 7
    $json|Set-Content -LiteralPath (Join-Path $dir 'result.json') -Encoding UTF8
    if($ResultFile) { $json|Set-Content -LiteralPath $ResultFile -Encoding UTF8 }
}
if(-not $result.Passed) { throw "Long playback did not pass: $($result.Error). Evidence: $dir" }
Write-Host "Continuous $Minutes-minute LHDC software playback verified: $dir"
