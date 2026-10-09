param(
    [ValidateSet('Playback','Call')][string]$Mode='Playback',
    [ValidateRange(1,120)][int]$Minutes=1,
    [switch]$Audible,
    [string]$ResultFile
)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$OutputEncoding=[Console]::OutputEncoding
$root=Split-Path -Parent $PSScriptRoot
$tool=Join-Path $root 'build/host/lhdc-host.exe'
$log=Join-Path $env:ProgramData 'LHDC-Win/service.jsonl'
$name=if ($Mode -eq 'Call') { 'call-switch' } else { 'long-playback' }
$dir=Join-Path $root ('evidence/local/'+$name+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $dir | Out-Null
$result=[ordered]@{StartedAt=Get-Date -Format o;Mode=$Mode;Completed=$false;Passed=$false;Error=$null;EvidenceDirectory=$dir;HearingVerified=$false;Silent=(-not $Audible);MicrophoneRecorded=$false}
$render=$null
$microphone=$null
$offset=$null
function Native([string[]]$Arguments) {
    $text=@(& $tool @Arguments)
    if ($LASTEXITCODE -ne 0) { throw "Backend query failed: $($Arguments[0])." }
    $text[-1] | ConvertFrom-Json
}
function Read-ServiceEvents([long]$StartOffset=$offset) {
    $file=[IO.FileStream]::new($log,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    try {
        $null=$file.Seek($StartOffset,[IO.SeekOrigin]::Begin)
        $reader=[IO.StreamReader]::new($file,[Text.Encoding]::UTF8)
        $text=$reader.ReadToEnd()
    } finally { $file.Dispose() }
    if ($StartOffset -eq $offset) { $text | Set-Content -LiteralPath (Join-Path $dir 'service.jsonl') -Encoding UTF8 }
    $text -split "`n" | Where-Object { $_.Trim() } | ForEach-Object { $_ | ConvertFrom-Json }
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
    if (-not $status.running) { throw 'LHDC service is not running.' }
    $result.ServiceHash=(Get-FileHash -LiteralPath (Join-Path $env:ProgramFiles 'LHDC-Win/lhdc-service.exe') -Algorithm SHA256).Hash
    if ($result.ServiceHash -ne (Get-FileHash -LiteralPath (Join-Path $root 'build/host/lhdc-service.exe') -Algorithm SHA256).Hash) {
        throw 'Install the completed service build before collecting playback metrics.'
    }
    $profile=(Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/LHDC-Win').Profile
    if ($profile.Length -ne 12) { throw 'Installed codec profile is invalid.' }
    $rate=[BitConverter]::ToUInt32($profile,0)
    $bits=[BitConverter]::ToUInt32($profile,4)
    if ($Mode -eq 'Call' -and $bits -ne 24) { throw 'This test verifies 24-bit music recovery; select 24-bit LHDC first.' }
    $devices=Native @('audio-devices')
    $source=@($devices.devices | Where-Object { $_.adapter_id -like '*lhdcwin#audio#*' -and $_.channels -eq 2 })
    if ($source.Count -ne 1 -or $source[0].device_sample_rate -ne $rate -or $source[0].device_bits -ne $bits) {
        throw 'The actual headphone endpoint differs from the selected format.'
    }
    $result.Before=$devices
    if ($Mode -eq 'Call') {
        $outputs=@($devices.devices | Where-Object container_id -EQ $source[0].container_id)
        if ($outputs.Count -ne 1) { throw 'Expected one native unified headphone playback endpoint.' }
        $result.NativeUnified=$true
    }
    $seconds=if ($Mode -eq 'Call') { 30 } else { 10 }
    $repeats=if ($Mode -eq 'Call') { 1 } else { $Minutes*6 }
    $wav=Join-Path $dir 'source.wav'
    $waveArguments=@('make-test-wav',$wav,"$seconds","$rate","$bits")
    if (-not $Audible) { $waveArguments+='--silent' }
    elseif ($Mode -eq 'Playback') { $waveArguments+='--loop' }
    $null=Native $waveArguments
    $offset=(Get-Item -LiteralPath $log).Length
    $render=Start-Process $tool -ArgumentList 'render-wav',$source[0].id,('"'+$wav+'"'),"$repeats" -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $dir 'render.jsonl') -RedirectStandardError (Join-Path $dir 'render.err')
    if ($Mode -eq 'Call') {
        Start-Sleep -Seconds 6
        $progress=@(Read-ServiceEvents | Where-Object event -EQ 'direct_audio_progress')
        $pcm=Native @('direct-pcm')
        if (-not $pcm.running -or $pcm.sample_rate -ne $rate -or $pcm.bits -ne $bits -or -not $progress.Count -or $progress[-1].packets -le 0) {
            throw 'LHDC music was not active at the selected real input format before microphone use.'
        }
        $result.BeforeCallPCM=$pcm
        $result.BeforeCallProgress=$progress[-1]
        $callOffset=(Get-Item -LiteralPath $log).Length
        $microphone=Start-Process $tool -ArgumentList 'call-probe','8' -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $dir 'microphone.jsonl') -RedirectStandardError (Join-Path $dir 'microphone.err')
        Start-Sleep -Seconds 4
        $result.DuringCall=Native @('direct-pcm')
        if ($result.DuringCall.running) { throw 'Windows did not switch the unified endpoint away from LHDC during microphone use.' }
        if (-not $microphone.WaitForExit(20000) -or $microphone.ExitCode -ne 0) { throw 'Microphone probe failed or timed out.' }
        if (-not $render.WaitForExit(35000) -or $render.ExitCode -ne 0) { throw 'Playback failed or timed out.' }
        Start-Sleep -Seconds 2
        $starts=@(Read-ServiceEvents | Where-Object event -EQ 'direct_audio_started')
        $resumed=@(Read-ServiceEvents -StartOffset $callOffset | Where-Object event -EQ 'direct_audio_started')
        if (-not $resumed.Count -or @($starts | Where-Object { $_.input_sample_rate -ne $rate -or $_.input_bits -ne $bits -or $_.resampling }).Count) {
            throw 'LHDC did not resume at the selected real input format after microphone use.'
        }
        $result.Microphone=@(Get-Content -LiteralPath (Join-Path $dir 'microphone.jsonl') -Encoding UTF8 | ForEach-Object { $_ | ConvertFrom-Json })
        $result.PlaybackSessions=$starts
    } else {
        $result.Minutes=$Minutes
        $deadline=(Get-Date).AddMinutes($Minutes).AddSeconds(20)
        while (-not $render.WaitForExit(10000)) {
            $pcm=Native @('direct-pcm')
            @{Time=Get-Date -Format o;PCM=$pcm} | ConvertTo-Json -Depth 4 -Compress | Add-Content -LiteralPath (Join-Path $dir 'samples.jsonl') -Encoding UTF8
            if (-not $pcm.running -or $pcm.sample_rate -ne $rate -or $pcm.bits -ne $bits) { throw 'Continuous PCM playback stopped or changed format.' }
            if ((Get-Date) -gt $deadline) { throw 'Long playback exceeded its deadline.' }
        }
        if ($render.ExitCode -ne 0) { throw 'Long playback renderer failed; see render.err.' }
        $result.Render=@(Get-Content -LiteralPath (Join-Path $dir 'render.jsonl') -Encoding UTF8 | ForEach-Object { $_ | ConvertFrom-Json })
        $completion=@($result.Render | Where-Object event -EQ 'wav_render_complete')
        if ($completion.Count -ne 1 -or $completion[0].frames -ne ($Minutes*60*$rate)) { throw 'The continuous renderer did not submit the full requested duration.' }
        Start-Sleep -Seconds 2
    }
    $result.After=Native @('audio-devices')
    $result.Completed=$true
} catch { $result.Error=$_.Exception.Message }
finally {
    foreach ($process in @($microphone,$render)) {
        if ($null -ne $process -and -not $process.HasExited) { $process.Kill();$process.WaitForExit() }
    }
    $sourceWav=Join-Path $dir 'source.wav'
    if (Test-Path -LiteralPath $sourceWav) { Remove-Item -LiteralPath $sourceWav }
    $sessions=@()
    $audioErrors=@()
    if ($null -ne $offset) {
        $events=@(Read-ServiceEvents)
        $sessions=@($events | Where-Object event -EQ 'direct_audio_complete')
        $audioErrors=@($events | Where-Object event -In @('service_audio_error','service_fatal_error','signalling_monitor_failed'))
        $result.Sessions=$sessions
        $result.AudioErrors=$audioErrors
        $result.SendStalls=@($events | Where-Object event -EQ 'media_send_stall')
        $progress=@($events | Where-Object event -EQ 'direct_audio_progress')
        if ($progress.Count) {
            $result.MaxSendUs=($progress | Measure-Object max_send_us -Maximum).Maximum
            $result.MaxReadGapUs=($progress | Measure-Object max_read_gap_us -Maximum).Maximum
            $result.MaxHfpCheckUs=($progress | Measure-Object max_hfp_check_us -Maximum).Maximum
        }
    }
    if ($Mode -eq 'Playback') {
        $result.FailureReasons=@(Get-LongPlaybackFailures -Completed $result.Completed -Sessions $sessions -AudioErrors $audioErrors -RuntimeError $result.Error)
    } else {
        $result.FailureReasons=@(
            if ($result.Error) { $result.Error }
            if ($audioErrors.Count) { "Service audio errors: $($audioErrors.Count); see service.jsonl." }
        )
    }
    $result.Passed=$result.Completed -and $result.FailureReasons.Count -eq 0
    if ($result.FailureReasons.Count) { $result.Error=$result.FailureReasons -join ' ' }
    $result.FinishedAt=Get-Date -Format o
    $json=$result | ConvertTo-Json -Depth 8
    $json | Set-Content -LiteralPath (Join-Path $dir 'result.json') -Encoding UTF8
    if ($ResultFile) { $json | Set-Content -LiteralPath $ResultFile -Encoding UTF8 }
}
if (-not $result.Passed) { throw "$Mode verification failed: $($result.Error). Evidence: $dir" }
Write-Host "$Mode verification passed: $dir"
