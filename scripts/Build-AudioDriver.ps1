param([switch]$Analyze)
$ErrorActionPreference='Stop'
Set-StrictMode -Version Latest
$projectRoot=Split-Path -Parent $PSScriptRoot
$buildMarker=Join-Path $projectRoot 'build/audio-driver/build-ready.json'
if (Test-Path -LiteralPath $buildMarker) { Remove-Item -LiteralPath $buildMarker }
$reference=Join-Path $projectRoot 'third_party/windows-driver-samples'
$revision=& git -C $reference rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $revision -ne '2dc3fd3a0cc84a2933f2194e7ec0871584979071') { throw 'Audio framework reference revision differs.' }
$prepared=Join-Path $projectRoot 'build/audio-reference'
New-Item -ItemType Directory -Path $prepared -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $reference 'audio/simpleaudiosample/Source') -Destination $prepared -Recurse -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'drivers/audio/minipairs.h') -Destination (Join-Path $prepared 'Source/Filters/minipairs.h') -Force
Copy-Item -LiteralPath (Join-Path $projectRoot 'drivers/audio/speakerwavtable.h') -Destination (Join-Path $prepared 'Source/Filters/speakerwavtable.h') -Force

function Replace-Source([string]$Relative,[string]$Pattern,[string]$Replacement,[int]$ExpectedCount=1) {
    $path=Join-Path $prepared $Relative
    $source=Get-Content -LiteralPath $path -Encoding UTF8 -Raw
    $matches=[regex]::Matches($source,$Pattern)
    if ($matches.Count -ne $ExpectedCount) { throw "Expected $ExpectedCount source block(s) for $Relative, found $($matches.Count)." }
    [regex]::Replace($source,$Pattern,$Replacement) | Set-Content -LiteralPath $path -Encoding UTF8
}
# Keep the vendor framework immutable. Prepare only the single render path.
# No microphone, sample tone or file recording exists.
$stream='Source/Main/minwavertstream.cpp'
Replace-Source $stream '(?s)    if \(m_pNotificationTimer\)\r?\n    \{\r?\n        ExDeleteTimer.*?\r?\n    \}' ''
Replace-Source $stream '(?s)    // Since we just cancelled the notification timer.*?    KeFlushQueuedDpcs\(\);' ''
Replace-Source $stream '    PAGED_CODE\(\);\r?\n    if \(NULL != m_pMiniport\)' @'
    PAGED_CODE();
    // A render callback can use the miniport and format; drain it before releasing either.
    if (m_pNotificationTimer) {
        ExDeleteTimer(m_pNotificationTimer, TRUE, TRUE, NULL);
        m_pNotificationTimer = NULL;
    }
    KeFlushQueuedDpcs();
    if (NULL != m_pMiniport)
'@
Replace-Source $stream '(?s)    if \(m_bCapture\)\r?\n    \{\r?\n        ReadRegistrySettings\(\);.*?(?=    //\r?\n    // Register this stream\.)' "    if (m_bCapture) return STATUS_NOT_SUPPORTED;`n`n"
Replace-Source $stream '(?s)    if \(!m_bCapture && \(!g_DoNotCreateDataFiles\)\).*?(?=    PHYSICAL_ADDRESS highAddress;)' ''
Replace-Source $stream '(?s)            // Wait until all work items are completed\..*?(?=            break;)' ''
Replace-Source $stream '(?s)        if \(!g_DoNotCreateDataFiles\).*?ReadBytes\(ByteDisplacement\);\r?\n        \}' '        ReadBytes(ByteDisplacement);'
Replace-Source $stream '(?s)    ULONG bufferOffset = m_ullLinearPosition % m_ulDmaBufferSize;.*?GenerateSine.*?\r?\n    \}\r?\n(?=\})' '    UNREFERENCED_PARAMETER(ByteDisplacement);'
Replace-Source $stream '(?s)    ULONG bufferOffset = m_ullLinearPosition % m_ulDmaBufferSize;.*?WriteData.*?\r?\n    \}\r?\n(?=\})' @'
    ULONG offset = (ULONG)(m_ullLinearPosition % m_ulDmaBufferSize);
    while (ByteDisplacement > 0) {
        ULONG bytes = min(ByteDisplacement, m_ulDmaBufferSize - offset);
        m_pMiniport->m_LhdcPcm.Push(m_pDmaBuffer + offset, bytes);
        offset = (offset + bytes) % m_ulDmaBufferSize;
        ByteDisplacement -= bytes;
    }
'@
Replace-Source 'Source/Main/minwavert.h' '#define _SIMPLEAUDIOSAMPLE_MINWAVERT_H_' "#define _SIMPLEAUDIOSAMPLE_MINWAVERT_H_`n#include `"pcm_bridge.h`""
Replace-Source 'Source/Main/minwavert.h' '(?m)^public:\r?\n    NTSTATUS EventHandler_PinCapsChange' "public:`n    LhdcPcmBridge m_LhdcPcm;`n    NTSTATUS EventHandler_PinCapsChange"
Replace-Source $stream '    ntStatus = m_pMiniport->StreamCreated\(m_ulPin, this\);' "    m_pMiniport->m_LhdcPcm.Configure(m_pWfExt);`n    ntStatus = m_pMiniport->StreamCreated(m_ulPin, this);"
Replace-Source $stream '    m_KsState = State_;' "    if (State_ != m_KsState) m_pMiniport->m_LhdcPcm.SetRunning(State_ == KSSTATE_RUN);`n    m_KsState = State_;"
Replace-Source $stream '    m_byteDisplacementCarryForward = \(\(m_ulDmaMovementRate \* TimeElapsedInMS\) \+ m_byteDisplacementCarryForward\) % 1000;' @'
    m_byteDisplacementCarryForward = ((m_ulDmaMovementRate * TimeElapsedInMS) + m_byteDisplacementCarryForward) % 1000;
    const ULONG partialFrame = ByteDisplacement % m_pWfExt->Format.nBlockAlign;
    ByteDisplacement -= partialFrame;
    m_byteDisplacementCarryForward += partialFrame * 1000;
'@
Replace-Source $stream '    m_SaveData.Disable\(drmRights->CopyProtect\);' '    m_pMiniport->m_LhdcPcm.SetProtected(drmRights->CopyProtect != FALSE);'
Replace-Source 'Source/Main/minwavertstream.h' '(?m)^#include "savedata.h"\r?\n#include "ToneGenerator.h"' ''
Replace-Source 'Source/Main/minwavertstream.h' '(?m)^    CSaveData +m_SaveData;\r?\n    ToneGenerator +m_ToneGenerator;' ''
# Topology volume/mute is backed by the service's acknowledged AVRCP commands.
# Windows must leave PCM master gain at unity; the headphones apply it once.
$topology='Source/Filters/speakertoptable.h'
# Windows derives the endpoint type, icon and localized name from this bridge pin.
Replace-Source $topology '&KSNODETYPE_SPEAKER' '&KSNODETYPE_HEADPHONES'
Replace-Source $topology '#define _SIMPLEAUDIOSAMPLE_SPEAKERTOPTABLE_H_' "#define _SIMPLEAUDIOSAMPLE_SPEAKERTOPTABLE_H_`n#include `"format_config.h`""
Replace-Source $topology '(?s)(PCPROPERTY_ITEM PropertiesSpeakerTopoFilter\[\] =\s+\{)' @'
$1
    {&KSPROPSETID_Jack,KSPROPERTY_JACK_DESCRIPTION3,
     KSPROPERTY_TYPE_GET|KSPROPERTY_TYPE_BASICSUPPORT,PropertyHandler_LhdcJackConfig},
'@
Replace-Source 'Source/Main/adapter.cpp' '    ntStatus = InstallAllRenderFilters\(DeviceObject, Irp, pAdapterCommon\);' @'
    ntStatus = LhdcLoadPlaybackFormat(&SpeakerHostPinSupportedDeviceFormats[0],&SpeakerPinDataRangesStream[0]);
    IF_FAILED_JUMP(ntStatus, Exit);
    ntStatus = InstallAllRenderFilters(DeviceObject, Irp, pAdapterCommon);
'@
# Absolute volume is one gain shared by both stereo channels.
Replace-Source 'Source/Main/basetopo.cpp' '(?s)(PropertyHandler_Volume\(\s+m_AdapterCommon,\s+PropertyRequest,\s+)m_DeviceMaxChannels' '${1}1'
Replace-Source 'Source/Main/basetopo.cpp' '(?s)(PropertyHandler_Mute\(\s+m_AdapterCommon,\s+PropertyRequest,\s+)m_DeviceMaxChannels' '${1}1'
# The sample incorrectly iterates to ALL_CHANNELS_ID rather than MaxChannels.
Replace-Source 'Source/Utilities/kshelper.cpp' 'for \(ULONG i=0; i<ulChannel; \+\+i\)' 'for (ULONG i=0; i<MaxChannels; ++i)' 2
Replace-Source 'Source/Main/adapter.cpp' '(?s)    NTSTATUS +ntStatus;\r?\n    PENDPOINT_MINIPAIR\* ppAeMiniports = g_CaptureEndpoints;.*?    return ntStatus;' "    PAGED_CODE();`n    UNREFERENCED_PARAMETER(_pDeviceObject);`n    UNREFERENCED_PARAMETER(_pIrp);`n    UNREFERENCED_PARAMETER(_pAdapterCommon);`n    return STATUS_SUCCESS;"
Replace-Source 'Source/Main/common.cpp' '(?m)^#include "savedata.h"' ''
Replace-Source 'Source/Main/common.cpp' '(?m)^PSAVEWORKER_PARAM +CSaveData::m_pWorkItems = NULL;\r?\nPDEVICE_OBJECT +CSaveData::m_pDeviceObject = NULL;' ''
Replace-Source 'Source/Main/common.cpp' '    CSaveData::DestroyWorkItems\(\);' ''
Replace-Source 'Source/Main/common.cpp' '(?s)    CSaveData::SetDeviceObject\(DeviceObject\);.*?    IF_FAILED_JUMP\(ntStatus, Done\);' ''
# Remove sample-only interface tests that register the current system thread.
Replace-Source 'Source/Main/adapter.cpp' '(?s)        //\r?\n        // Test: add and remove current thread as streaming audio resource\..*?(?=    \}\r?\n\r?\n    SAFE_RELEASE\(unknownTopology\))' ''
Replace-Source 'Source/Main/adapter.cpp' '(?m)^    PPORTCLSStreamResourceManager +pPortClsResMgr *= NULL;\r?\n    PPORTCLSStreamResourceManager2 +pPortClsResMgr2 *= NULL;' ''
Replace-Source 'Source/Main/minwavert.cpp' '(?s)    if \(\(this->m_DeviceType\) == eMicArrayDevice1\).*?    else\r?\n    \{\r?\n        requiredSize =' "    UNREFERENCED_PARAMETER(ResultantFormat);`n    {`n        requiredSize ="
# Return the exact selected tuple instead of delegating the sample's range
# intersection to PortCls, which only returns a legacy WAVEFORMATEX structure.
Replace-Source 'Source/Main/minwavert.cpp' '(?s)    //If called for the mic array pin.*?(?=\r?\n\} // DataRangeIntersection)' @'
    if (PinId != KSPIN_WAVE_RENDER3_SINK_SYSTEM ||
        ClientDataRange->FormatSize < sizeof(KSDATARANGE_AUDIO) ||
        MyDataRange->FormatSize < sizeof(KSDATARANGE_AUDIO)) return STATUS_NO_MATCH;
    PKSDATAFORMAT_WAVEFORMATEXTENSIBLE formats = nullptr;
    const auto count = GetPinSupportedDeviceFormats(PinId, &formats);
    auto* client = reinterpret_cast<PKSDATARANGE_AUDIO>(ClientDataRange);
    PKSDATAFORMAT_WAVEFORMATEXTENSIBLE selected = nullptr;
    for (ULONG index = 0; index < count; ++index) {
        const auto& wave = formats[index].WaveFormatExt.Format;
        if ((!IsEqualGUIDAligned(ClientDataRange->SubFormat, formats[index].DataFormat.SubFormat) &&
         !IsEqualGUIDAligned(ClientDataRange->SubFormat, KSDATAFORMAT_SUBTYPE_WILDCARD)) ||
        client->MaximumChannels < wave.nChannels ||
        client->MinimumBitsPerSample > wave.wBitsPerSample || client->MaximumBitsPerSample < wave.wBitsPerSample ||
        client->MinimumSampleFrequency > wave.nSamplesPerSec || client->MaximumSampleFrequency < wave.nSamplesPerSec)
            continue;
        selected = &formats[index]; break;
    }
    if (!selected) return STATUS_NO_MATCH;
    requiredSize = sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    *ResultantFormatLength = requiredSize;
    if (!OutputBufferLength) return STATUS_BUFFER_OVERFLOW;
    if (OutputBufferLength < requiredSize) return STATUS_BUFFER_TOO_SMALL;
    if (!ResultantFormat) return STATUS_INVALID_PARAMETER;
    RtlCopyMemory(ResultantFormat, selected, requiredSize);
    return STATUS_SUCCESS;
'@
# Explicitly initialize framework fields. ExAllocatePool2 already zeroes these
# objects, but constructors should not rely on allocation for their initial state.
foreach ($relative in @('Source/Main/common.cpp','Source/Main/minwavert.h','Source/Main/minwavertstream.h')) {
    $path=Join-Path $prepared $relative
    $source=Get-Content -LiteralPath $path -Encoding UTF8 -Raw
    $pattern='(?m)^([ \t]+(?:[A-Z]\w*|eDeviceType)[ \t]*\*?[ \t]+m_\w+)[ \t]*;'
    if ([regex]::Matches($source,$pattern).Count -eq 0) { throw "No framework fields found in $relative." }
    [regex]::Replace($source,$pattern,'$1{};') | Set-Content -LiteralPath $path -Encoding UTF8
}

$installation=& 'C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe' -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $installation) { throw 'MSVC x64 toolchain not found.' }
Import-Module "$installation/Common7/Tools/Microsoft.VisualStudio.DevShell.dll"
Enter-VsDevShell -VsInstallPath $installation -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$analysisArguments=@()
if ($Analyze) {
    $kitsRoot=(Get-ItemProperty -LiteralPath 'HKLM:/SOFTWARE/Microsoft/Windows Kits/Installed Roots').KitsRoot10
    $analysisArguments=@('/p:RunCodeAnalysis=true',('/p:CodeAnalysisRuleSet='+ (Join-Path $kitsRoot 'CodeAnalysis/DriverMinimumRules.ruleset')))
}
& "$installation/MSBuild/Current/Bin/MSBuild.exe" (Join-Path $projectRoot 'drivers/audio/lhdc-audio.vcxproj') /m /p:Configuration=Release /p:Platform=x64 /verbosity:minimal @analysisArguments
if ($LASTEXITCODE -ne 0) { throw 'Virtual audio driver build failed.' }
[pscustomobject]@{BuiltAt=(Get-Date -Format o);Revision=$revision;Files=@(Get-FileHash -LiteralPath (Join-Path $projectRoot 'build/audio-driver/lhdc-audio/lhdc-audio.sys'),(Join-Path $projectRoot 'build/audio-driver/lhdc-audio/lhdc-audio.inf') -Algorithm SHA256 | Select-Object Path,Hash)} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $buildMarker -Encoding UTF8
