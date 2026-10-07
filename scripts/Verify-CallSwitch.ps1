param([string]$ResultFile,[switch]$Audible)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$OutputEncoding=[Console]::OutputEncoding
$root=Split-Path -Parent $PSScriptRoot
$hostTool=Join-Path $root 'build/host/lhdc-host.exe'
$log=Join-Path $env:ProgramData 'LHDC-Win/service.jsonl'
$evidence=Join-Path $root ('evidence/local/call-switch-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $evidence | Out-Null
$result=[ordered]@{StartedAt=Get-Date -Format o;Completed=$false;Error=$null;EvidenceDirectory=$evidence;HearingVerified=$false;MicrophoneRecorded=$false;TestPlaybackSilent=(-not $Audible)}
$render=$null
$microphone=$null
$offset=$null
function Native([string[]]$Arguments) {
    $output=@(& $hostTool @Arguments)
    if($LASTEXITCODE -ne 0) { throw "Backend query failed: $($Arguments[0])." }
    $output[-1] | ConvertFrom-Json
}
function Read-ServiceEvents([long]$StartOffset=$offset) {
    $file=[IO.FileStream]::new($log,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
    try {
        $null=$file.Seek($StartOffset,[IO.SeekOrigin]::Begin)
        $reader=[IO.StreamReader]::new($file,[Text.Encoding]::UTF8)
        $text=$reader.ReadToEnd()
    } finally { $file.Dispose() }
    if($StartOffset -eq $offset) { $text|Set-Content -LiteralPath (Join-Path $evidence 'service.jsonl') -Encoding UTF8 }
    $text -split "`n"|Where-Object { $_.Trim() }|ForEach-Object { $_|ConvertFrom-Json }
}
try {
    $profile=(Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/LHDC-Win').Profile
    if($profile.Length -ne 12) { throw 'Installed codec profile is invalid.' }
    $rate=[BitConverter]::ToUInt32($profile,0)
    $bits=[BitConverter]::ToUInt32($profile,4)
    if($bits -ne 24) { throw 'This test verifies 24-bit music recovery; select 24-bit LHDC first.' }
    $service=Native @('audio-status')
    if(-not $service.running) { throw 'LHDC service is not running.' }
    $devices=Native @('audio-devices')
    $source=@($devices.devices | Where-Object { $_.adapter_id -like '*lhdcwin#audio#*' -and $_.channels -eq 2 })
    if($source.Count -ne 1 -or $source[0].device_sample_rate -ne $rate -or $source[0].device_bits -ne $bits) { throw 'The actual headphone endpoint differs from the selected format.' }
    $result.Before=$devices
    $outputs=@($devices.devices|Where-Object container_id -EQ $source[0].container_id)
    $result.NativeUnified=$outputs.Count -eq 1
    if(-not $result.NativeUnified) { throw 'Expected one native unified headphone playback endpoint.' }
    $offset=(Get-Item -LiteralPath $log).Length
    $wav=Join-Path $evidence 'source.wav'
    if($Audible) {
        $null=Native @('make-test-wav',$wav,'30',"$rate","$bits")
    } else {
    $size=30*$rate*2*($bits/8)
    $file=[IO.File]::Create($wav)
    $writer=[IO.BinaryWriter]::new($file,[Text.Encoding]::UTF8)
    try {
        $writer.Write([Text.Encoding]::ASCII.GetBytes('RIFF'));$writer.Write([uint32](36+$size))
        $writer.Write([Text.Encoding]::ASCII.GetBytes('WAVEfmt '));$writer.Write([uint32]16)
        $writer.Write([uint16]1);$writer.Write([uint16]2);$writer.Write([uint32]$rate)
        $writer.Write([uint32]($rate*2*($bits/8)));$writer.Write([uint16](2*($bits/8)));$writer.Write([uint16]$bits)
        $writer.Write([Text.Encoding]::ASCII.GetBytes('data'));$writer.Write([uint32]$size)
        $writer.Write([byte[]]::new($size))
    } finally { $writer.Dispose() }
    }
    $render=Start-Process -FilePath $hostTool -ArgumentList 'render-wav',$source[0].id,('"'+$wav+'"') -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $evidence 'render.jsonl') -RedirectStandardError (Join-Path $evidence 'render.err')
    Start-Sleep -Seconds 6
    $beforeCallEvents=@(Read-ServiceEvents)
    $result.BeforeCallPCM=Native @('direct-pcm')
    $progress=@($beforeCallEvents|Where-Object event -EQ 'direct_audio_progress')
    if(-not $result.BeforeCallPCM.running -or $result.BeforeCallPCM.sample_rate -ne $rate -or $result.BeforeCallPCM.bits -ne $bits -or -not $progress.Count -or $progress[-1].packets -le 0) {
        throw 'LHDC music was not active at the selected real input format before microphone use.'
    }
    $result.BeforeCallProgress=$progress[-1]
    $callOffset=(Get-Item -LiteralPath $log).Length
    $microphone=Start-Process -FilePath $hostTool -ArgumentList 'call-probe','8' -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $evidence 'microphone.jsonl') -RedirectStandardError (Join-Path $evidence 'microphone.err')
    Start-Sleep -Seconds 4
    $result.DuringCall=Native @('direct-pcm')
    if($result.DuringCall.running) { throw 'Windows did not switch the unified endpoint away from LHDC during microphone use.' }
    if(-not $microphone.WaitForExit(20000) -or $microphone.ExitCode -ne 0) { throw 'Microphone probe failed or timed out.' }
    if(-not $render.WaitForExit(35000) -or $render.ExitCode -ne 0) { throw 'Playback failed or timed out.' }
    Start-Sleep -Seconds 2
    $events=@(Read-ServiceEvents)
    $starts=@($events | Where-Object event -EQ 'direct_audio_started')
    # A stream already playing before the test has no new start in this log slice.
    $resumeEvents=@(Read-ServiceEvents -StartOffset $callOffset)
    $resumed=@($resumeEvents | Where-Object event -EQ 'direct_audio_started')
    if(-not $resumed.Count -or @($starts | Where-Object { $_.input_sample_rate -ne $rate -or $_.input_bits -ne $bits -or $_.resampling }).Count) { throw 'LHDC did not resume at the selected real input format after microphone use.' }
    if(@($events | Where-Object { $_.event -in @('service_audio_error','service_fatal_error') }).Count) { throw 'The service reported an audio error during the call test.' }
    $result.Microphone=@(Get-Content -LiteralPath (Join-Path $evidence 'microphone.jsonl') -Encoding UTF8 | ForEach-Object { $_ | ConvertFrom-Json })
    $result.PlaybackSessions=$starts
    $result.After=Native @('audio-devices')
    $result.Completed=$true
} catch {
    $result.Error=$_.Exception.Message
    $result.ErrorLocation=$_.ScriptStackTrace
} finally {
    foreach($process in @($microphone,$render)) { if($null -ne $process -and -not $process.HasExited) { $process.Kill();$process.WaitForExit() } }
    $sourceWav=Join-Path $evidence 'source.wav'
    if(Test-Path -LiteralPath $sourceWav) { Remove-Item -LiteralPath $sourceWav }
    if($null -ne $offset) {
        $events=@(Read-ServiceEvents)
        $result.AudioErrors=@($events|Where-Object event -In @('service_audio_error','service_fatal_error','signalling_monitor_failed'))
    }
    $result.FinishedAt=Get-Date -Format o
    $json=$result | ConvertTo-Json -Depth 8
    $json | Set-Content -LiteralPath (Join-Path $evidence 'result.json') -Encoding UTF8
    if($ResultFile) { $json | Set-Content -LiteralPath $ResultFile -Encoding UTF8 }
}
if(-not $result.Completed) { throw "Call verification failed: $($result.Error). Evidence: $evidence" }
Write-Host "Automatic call routing and 24-bit music recovery verified: $evidence"
