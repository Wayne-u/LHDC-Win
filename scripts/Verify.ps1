$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false)
$OutputEncoding=[Console]::OutputEncoding
Set-StrictMode -Version Latest
$projectRoot=Split-Path -Parent $PSScriptRoot
$output=Join-Path $projectRoot ('evidence/local/offline-'+(Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$hostExe=Join-Path $projectRoot 'build\host\lhdc-host.exe'
$refExe=Join-Path $projectRoot 'build\host\third_party\lhdcv5\ref_encode_c.exe'
New-Item -ItemType Directory -Path $output | Out-Null
$scratch=Join-Path $output 'scratch'
New-Item -ItemType Directory -Path $scratch | Out-Null
function Read-Records([string]$Path,[uint32]$Mtu) {
    $stream=[IO.File]::OpenRead($Path)
    $reader=[IO.BinaryReader]::new($stream)
    $records=0; $packets=0; $frames=0; $payloadBytes=0; $maxPacket=0
    try {
            while ($stream.Position -lt $stream.Length) {
                if ($stream.Length-$stream.Position -lt 8) { throw 'Truncated record header' }
                $length=$reader.ReadUInt32(); $count=$reader.ReadUInt32()
                if ($length -gt $Mtu -or (($length -eq 0) -ne ($count -eq 0)) -or $count -gt 63) { throw 'Invalid packet boundary or frame count' }
                if ($length -gt $stream.Length-$stream.Position) { throw 'Truncated record payload' }
                $stream.Position+=$length
                $records++; $frames+=$count; $payloadBytes+=$length
                if ($length) { $packets++ }
                $maxPacket=[Math]::Max($maxPacket,$length)
            }
        } finally { $reader.Dispose() }
        [pscustomobject]@{Records=$records;Packets=$packets;Frames=$frames;PayloadBytes=$payloadBytes;MaxPacket=$maxPacket}
    }
    function Invoke-HostJson([string[]]$Arguments) {
        $lines=@(& $hostExe @Arguments 2>&1 | ForEach-Object { $_.ToString() })
        if ($LASTEXITCODE -ne 0) { throw "Host failed: $lines" }
        $lines[-1] | ConvertFrom-Json
    }
    function Read-BigEndian([byte[]]$Bytes,[int]$Offset,[int]$Count) {
        [uint64]$value=0
        for ($i=0;$i -lt $Count;$i++) { $value=($value -shl 8) -bor $Bytes[$Offset+$i] }
        $value
    }
    function Test-MediaRecords([string]$Path,$Summary) {
        $reader=[IO.BinaryReader]::new([IO.File]::OpenRead($Path))
        [uint64]$frames=0
        [int]$packets=0
        [int]$blocks=0
        try {
            while ($reader.BaseStream.Position -lt $reader.BaseStream.Length) {
                $size=$reader.ReadUInt32(); $count=$reader.ReadUInt32(); $blocks++
                if ($size -eq 0) {
                    if ($count -ne 0) { throw 'Empty encoder output has a nonzero frame count.' }
                    continue
                }
                if ($size -gt 672 -or $size -le 14 -or $count -lt 1 -or $count -gt 63) { throw 'Invalid packet length/count.' }
                $packet=$reader.ReadBytes($size)
                if ($packet.Length -ne $size) { throw 'Truncated packet.' }
                if ($packet[0] -ne 128 -or $packet[1] -ne 96 -or (Read-BigEndian $packet 8 4) -ne 1) { throw 'Invalid RTP version, payload type or SSRC.' }
                if ((Read-BigEndian $packet 2 2) -ne ($packets % 65536) -or $packet[13] -ne ($packets % 256)) { throw 'Media sequence is discontinuous.' }
                if ((Read-BigEndian $packet 4 4) -ne (($frames*$Summary.block_samples) % 4294967296)) { throw 'RTP sample timestamp is discontinuous.' }
                if ($packet[12] -ne ($count -shl 2)) { throw 'LHDC header frame count differs from record.' }
                $frames+=$count; $packets++
            }
        } finally { $reader.Dispose() }
        if ($frames -ne $Summary.emitted_frames -or $packets -ne $Summary.nonempty_packets -or $blocks -ne $Summary.encode_calls -or ($blocks-$frames) -ne $Summary.pending_frames_at_eof) { throw 'Record totals differ from encoder statistics.' }
        if ($frames -lt $Summary.input_blocks -or $Summary.pending_frames_at_eof -ne 0 -or $blocks -ne ($Summary.input_blocks+$Summary.silent_padding_blocks)) { throw 'Final input frames were not completely emitted or silent padding was not accounted for.' }
    }
    try {
    $vectors=Get-Content -LiteralPath (Join-Path $projectRoot 'tests\vectors\encoder-hashes.json') -Encoding UTF8 | ConvertFrom-Json
    $revision=git -C (Join-Path $projectRoot 'third_party\lhdcv5') rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $revision -ne $vectors.source_commit) { throw 'Encoder revision differs from regression vectors' }
    $results=foreach ($case in $vectors.cases) {
        $path=Join-Path $scratch ($case.name+'.bin')
        $arguments=@($case.rate,$case.bits,$case.index,500,$path)
        if ($case.PSObject.Properties.Name -contains 'switch') { $arguments+=$case.switch }
        & $refExe @arguments | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Reference harness failed: $($case.name)" }
        $hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash
        if ($hash -ne $case.sha256) { throw "MSVC differs from prior C/Clang vector: $($case.name)" }
        $records=Read-Records $path 660
        if ($records.Records -ne 500 -or $records.Frames -ne 500) { throw 'Reference frame totals differ' }
        [pscustomobject]@{Name=$case.name;Sha256=$hash;Records=$records}
    }
    $negative=foreach ($parameters in @(@(32000,16,5),@(48000,32,5),@(48000,16,11))) {
        & $refExe @parameters 500 (Join-Path $scratch 'rejected.bin')
        $code=$LASTEXITCODE
        if ($code -ne 1) { throw "Invalid configuration was not rejected: $parameters" }
        [pscustomobject]@{Parameters=$parameters;ExitCode=$code}
    }
    $wav=Join-Path $scratch 'tone-60s.wav'
    $encoded=Join-Path $scratch 'tone-60s.records'
    & $hostExe make-test-wav $wav 60
    if ($LASTEXITCODE -ne 0) { throw 'Test WAV creation failed' }
    $statsText=& $hostExe encode $wav $encoded
    if ($LASTEXITCODE -ne 0) { throw 'WAV encoding failed' }
    $stats=$statsText | ConvertFrom-Json
    $parsed=Read-Records $encoded 660
    if ($parsed.Records -ne 12000 -or $parsed.Frames -ne 12000 -or $stats.pending_frames_at_eof -ne 0) { throw '60s encoder frame totals differ' }
    if ($stats.encode_us.p99 -ge 2000) { throw 'Encoding exceeds the development p99 target of 2ms' }
    $caps='0100070d00ff3a050000354c3016114000'
    $options=Invoke-HostJson @('profile-options',$caps)
    $resultsMedia=[Collections.Generic.List[object]]::new()
    foreach ($rate in $options.rates) {
        foreach ($bits in $options.bits) {
            # The Unicode path also verifies UTF-8 arguments through the native Windows entry point.
            $wav=Join-Path $scratch "测试音-$($rate.sample_rate)-$bits.wav"
            Invoke-HostJson @('make-test-wav',$wav,'2',"$($rate.sample_rate)","$bits") | Out-Null
            foreach ($kbps in $rate.bitrates) {
                $name="$($rate.sample_rate)-$bits-$kbps"
                $validation=Invoke-HostJson @('configure',$caps,"$($rate.sample_rate)","$bits","$kbps")
                $records=Join-Path $scratch ($name+'.records')
                $summary=Invoke-HostJson @('packetize',$wav,$records,'672',"$kbps")
                Test-MediaRecords $records $summary
                $resultsMedia.Add([pscustomobject]@{Profile=$name;Configuration=$validation.configuration;Summary=$summary;RecordSHA256=(Get-FileHash -LiteralPath $records -Algorithm SHA256).Hash})
            }
        }
    }
    [pscustomobject]@{
        VerifiedAt=(Get-Date -Format o)
        EncoderRevision=$revision
        EncoderVectors=$results
        NegativeConfigurations=$negative
        OfflineEncoding=$stats
        MediaProfiles=$resultsMedia.ToArray()
        MediaMtuBudget=672
        BluetoothPlaybackTested=$false
    } | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'result.json') -Encoding UTF8
} finally {
    $resolvedScratch=(Resolve-Path -LiteralPath $scratch).Path
    if ($resolvedScratch -ne (Join-Path $output 'scratch') -or -not $resolvedScratch.StartsWith($projectRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) {
        throw 'Scratch directory is outside the verification output.'
    }
    Remove-Item -LiteralPath $resolvedScratch -Recurse -Force
}
Write-Host "Verified $($results.Count) encoder vectors, $($negative.Count) rejected configurations, 60s offline encoding and $($resultsMedia.Count) RTP/LHDC profiles. Evidence: $output"
