#include "definitions.h"
#include "endpoints.h"
#include "format_config.h"
namespace {
ULONG config_id=0;
KSDATAFORMAT_WAVEFORMATEXTENSIBLE* playback_format=nullptr;
}
#pragma code_seg("PAGE")
NTSTATUS LhdcLoadPlaybackFormat(KSDATAFORMAT_WAVEFORMATEXTENSIBLE* format,KSDATARANGE_AUDIO* range) {
    PAGED_CODE();
    UNICODE_STRING path,name;
    OBJECT_ATTRIBUTES attributes;
    HANDLE key;
    RtlInitUnicodeString(&path,L"\\Registry\\Machine\\SOFTWARE\\LHDC-Win");
    InitializeObjectAttributes(&attributes,&path,OBJ_KERNEL_HANDLE|OBJ_CASE_INSENSITIVE,nullptr,nullptr);
    auto status=ZwOpenKey(&key,KEY_QUERY_VALUE,&attributes);
    if (!NT_SUCCESS(status)) return status;
    RtlInitUnicodeString(&name,L"Profile");
    alignas(KEY_VALUE_PARTIAL_INFORMATION) BYTE buffer[FIELD_OFFSET(KEY_VALUE_PARTIAL_INFORMATION,Data)+3*sizeof(ULONG)]{};
    ULONG required=0;
    status=ZwQueryValueKey(key,&name,KeyValuePartialInformation,buffer,sizeof(buffer),&required);
    ZwClose(key);
    if (!NT_SUCCESS(status)) return status;
    auto* value=reinterpret_cast<KEY_VALUE_PARTIAL_INFORMATION*>(buffer);
    if (value->Type!=REG_BINARY || value->DataLength!=3*sizeof(ULONG)) return STATUS_DEVICE_CONFIGURATION_ERROR;
    ULONG profile[3];RtlCopyMemory(profile,value->Data,sizeof(profile));
    const auto rate=profile[0],bits=profile[1];
    // This target's real SDP capabilities are 44.1/48 kHz and 16/24-bit.
    if ((rate!=44100 && rate!=48000) || (bits!=16 && bits!=24)) return STATUS_DEVICE_CONFIGURATION_ERROR;
    auto& wave=format->WaveFormatExt;
    const auto inputBits=bits;
    const auto subtype=KSDATAFORMAT_SUBTYPE_PCM;
    format->DataFormat.SubFormat=wave.SubFormat=range->DataRange.SubFormat=subtype;
    wave.Format.nSamplesPerSec=rate;
    wave.Format.wBitsPerSample=static_cast<WORD>(inputBits);
    wave.Samples.wValidBitsPerSample=static_cast<WORD>(bits);
    wave.Format.nBlockAlign=static_cast<WORD>(2*(inputBits/8));
    wave.Format.nAvgBytesPerSec=rate*wave.Format.nBlockAlign;
    format->DataFormat.SampleSize=wave.Format.nBlockAlign;
    range->MinimumSampleFrequency=range->MaximumSampleFrequency=rate;
    range->MinimumBitsPerSample=range->MaximumBitsPerSample=inputBits;
    // Stable configuration identity: sample rate in upper bits, depth in low byte.
    config_id=0x02000000|(rate<<8)|bits;
    playback_format=format;
    return STATUS_SUCCESS;
}
NTSTATUS PropertyHandler_LhdcModeFormats(PPCPROPERTY_REQUEST request) {
    PAGED_CODE();
    if (request->Verb&KSPROPERTY_TYPE_BASICSUPPORT)
        return PropertyHandler_BasicSupport(request,KSPROPERTY_TYPE_GET|KSPROPERTY_TYPE_BASICSUPPORT,VT_ILLEGAL);
    struct ModeInstance { ULONG PinId; ULONG Reserved; GUID Mode; };
    if (!(request->Verb&KSPROPERTY_TYPE_GET) || !request->Instance ||
        request->InstanceSize<sizeof(ModeInstance)) return STATUS_INVALID_PARAMETER;
    auto* instance=static_cast<ModeInstance*>(request->Instance);
    if (instance->PinId!=KSPIN_WAVE_RENDER3_SINK_SYSTEM || !playback_format)
        return STATUS_INVALID_PARAMETER;
    if (!IsEqualGUIDAligned(instance->Mode,AUDIO_SIGNALPROCESSINGMODE_DEFAULT) &&
        !IsEqualGUIDAligned(instance->Mode,AUDIO_SIGNALPROCESSINGMODE_RAW)) return STATUS_NOT_SUPPORTED;
    // Endpoint Builder consumes ULONG offsets relative to the entries after
    // KSMULTIPLE_ITEM. Keep the descriptor itself aligned to eight bytes.
    const ULONG offset=sizeof(KSMULTIPLE_ITEM)+sizeof(ULONGLONG);
    const ULONG bytes=offset+sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE);
    const auto status=ValidatePropertyParams(request,bytes,sizeof(ModeInstance));
    if (!NT_SUCCESS(status)) return status;
    auto* items=static_cast<KSMULTIPLE_ITEM*>(request->Value);
    items->Size=bytes;items->Count=1;
    RtlZeroMemory(items+1,sizeof(ULONGLONG));
    *reinterpret_cast<ULONG*>(items+1)=offset-sizeof(KSMULTIPLE_ITEM);
    RtlCopyMemory(static_cast<BYTE*>(request->Value)+offset,playback_format,sizeof(*playback_format));
    request->ValueSize=bytes;
    return STATUS_SUCCESS;
}
NTSTATUS PropertyHandler_LhdcJackConfig(PPCPROPERTY_REQUEST request) {
    PAGED_CODE();
    if (request->Verb&KSPROPERTY_TYPE_BASICSUPPORT)
        return PropertyHandler_BasicSupport(request,KSPROPERTY_TYPE_GET|KSPROPERTY_TYPE_BASICSUPPORT,VT_ILLEGAL);
    if (!(request->Verb&KSPROPERTY_TYPE_GET) || request->InstanceSize<sizeof(ULONG) || !request->Instance ||
        *static_cast<ULONG*>(request->Instance)!=KSPIN_TOPO_LINEOUT_DEST) return STATUS_INVALID_PARAMETER;
    const ULONG bytes=sizeof(KSMULTIPLE_ITEM)+sizeof(KSJACK_DESCRIPTION3);
    const auto status=ValidatePropertyParams(request,bytes,sizeof(ULONG));
    if (!NT_SUCCESS(status)) return status;
    auto* items=static_cast<KSMULTIPLE_ITEM*>(request->Value);
    items->Size=bytes;items->Count=1;
    reinterpret_cast<KSJACK_DESCRIPTION3*>(items+1)->ConfigId=config_id;
    request->ValueSize=bytes;return STATUS_SUCCESS;
}
#pragma code_seg()
