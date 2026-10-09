param([ValidateSet(44100,48000,96000,192000)][int[]]$SampleRates=@(44100,48000),[string]$ResultFile)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$OutputEncoding=[Console]::OutputEncoding
$root=Split-Path -Parent $PSScriptRoot
$tool=Join-Path $root 'build/host/lhdc-host.exe'
$dir=Join-Path $root ('evidence/local/unified-pcm-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $dir|Out-Null
$profile=(Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/LHDC-Win').Profile
$originalRate=[BitConverter]::ToUInt32($profile,0)
$originalBits=[BitConverter]::ToUInt32($profile,4)
$originalKbps=[BitConverter]::ToUInt32($profile,8)
$result=[ordered]@{Completed=$false;Error=$null;RestoreError=$null;EvidenceDirectory=$dir;BluetoothPlaybackTested=$false;HearingVerified=$false;Cases=@()}
$render=$null
function Native([string[]]$Arguments) {
    $text=@(& $tool @Arguments)
    if($LASTEXITCODE) { throw "Backend failed: $($Arguments[0])." }
    $text[-1]|ConvertFrom-Json
}
$originalAdaptive=(Native @('audio-status')).adaptive_bitrate
try {
    foreach($rate in $SampleRates) { foreach($bits in @(16,24)) {
        $name="$rate-$bits"
        & (Join-Path $PSScriptRoot 'Set-AudioMode.ps1') -Mode LHDC -SampleRate $rate -Bits $bits -Kbps $originalKbps -ResultFile (Join-Path $dir ("install-$name.json"))
        Stop-Service -Name 'LHDC-Win'
        (Get-Service -Name 'LHDC-Win').WaitForStatus('Stopped',[TimeSpan]::FromSeconds(20))
        # Isolate Windows -> WaveRT PCM from Bluetooth transport and earbud wear.
        $devices=Native @('audio-devices')
        $source=@($devices.devices|Where-Object adapter_id -Like '*lhdcwin#audio#*')
        if($source.Count -ne 1) { throw 'Expected one LHDC music endpoint.' }
        $outputs=@($devices.devices|Where-Object container_id -EQ $source[0].container_id)
        if($outputs.Count -ne 1 -or $source[0].device_bits -ne $bits -or $source[0].device_sample_rate -ne $rate) {
            throw "Native single playback endpoint failed at $name."
        }
        $wav=Join-Path $dir ("source-$name.wav")
        $null=Native @('make-test-wav',$wav,'10',"$rate","$bits")
        $render=Start-Process $tool -ArgumentList 'render-wav',$source[0].id,('"'+$wav+'"') -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $dir ("render-$name.jsonl")) -RedirectStandardError (Join-Path $dir ("render-$name.err"))
        Start-Sleep -Milliseconds 300
        $pcm=Native @('direct-pcm')
        if(-not $pcm.running -or $pcm.sample_rate -ne $rate -or $pcm.bits -ne $bits -or $pcm.container_bits -ne $bits -or $pcm.floating_point) {
            throw "Actual WaveRT input failed at $name."
        }
        $raw=Join-Path $dir ("capture-$name.pcm")
        $capture=Native @('direct-capture','6',$raw)
        $capture|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $dir ("capture-$name.json")) -Encoding UTF8
        if($capture.frames -lt ($rate*5.9) -or $capture.frames -gt ($rate*6.1) -or $capture.peak -lt 0.030 -or $capture.peak -gt 0.033) {
            throw "PCM duration or level changed at $name."
        }
        $lowBytes=0
        if($bits -eq 24) {
            $data=[IO.File]::ReadAllBytes($raw)
            for($index=0;$index -lt $data.Length;$index+=3) { if($data[$index]) { ++$lowBytes } }
            if(-not $lowBytes) { throw '24-bit PCM contains only padded 16-bit samples.' }
        }
        if(-not $render.WaitForExit(15000) -or $render.ExitCode) { throw "Renderer failed at $name." }
        $render=$null
        $result.Cases+=@{Rate=$rate;Bits=$bits;Endpoint=$source[0];PCM=$pcm;Capture=$capture;NonzeroLowBytes=$lowBytes;NativeSinglePlaybackEndpoint=$true}
        Remove-Item -LiteralPath $wav,$raw
    } }
    $result.Completed=$true
} catch { $result.Error=$_.Exception.Message }
finally {
    if($null -ne $render -and -not $render.HasExited) { $render.Kill();$render.WaitForExit() }
    try {
        & (Join-Path $PSScriptRoot 'Set-AudioMode.ps1') -Mode LHDC -SampleRate $originalRate -Bits $originalBits -Kbps $originalKbps -AdaptiveBitrate:$originalAdaptive -ResultFile (Join-Path $dir 'restore.json')
    } catch { $result.RestoreError=$_.Exception.Message }
    $result.FinishedAt=Get-Date -Format o
    $json=$result|ConvertTo-Json -Depth 8
    $json|Set-Content -LiteralPath (Join-Path $dir 'result.json') -Encoding UTF8
    if($ResultFile) { $json|Set-Content -LiteralPath $ResultFile -Encoding UTF8 }
}
if(-not $result.Completed -or $result.RestoreError) { throw "Unified PCM verification failed: $($result.Error); restore: $($result.RestoreError). Evidence: $dir" }
Write-Host "Four native single playback formats and actual PCM verified: $dir"
