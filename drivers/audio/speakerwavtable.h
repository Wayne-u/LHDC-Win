// Copyright (c) Microsoft Corporation. All Rights Reserved.
// Derived from Simple Audio Sample, licensed under MS-PL.
// The panel selects one format before the audio child starts. Windows 11's
// unified Bluetooth endpoint otherwise ignores its default-format setting.
#pragma once
#include "pcm_bridge.h"
#include "format_config.h"
#define SPEAKER_DEVICE_MAX_CHANNELS 2
#define SPEAKER_MAX_INPUT_SYSTEM_STREAMS 1
// A literal descriptor macro keeps supported formats consistent.
#define LHDC_AUDIO_FORMAT(Rate,Bits,Subtype) \
    {{sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE),0,0,0, \
      STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),{STATIC_##Subtype}, \
      STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)}, \
     {{WAVE_FORMAT_EXTENSIBLE,2,Rate,Rate*2*(Bits/8),2*(Bits/8),Bits, \
       sizeof(WAVEFORMATEXTENSIBLE)-sizeof(WAVEFORMATEX)},Bits, \
       KSAUDIO_SPEAKER_STEREO,{STATIC_##Subtype}}}
static KSDATAFORMAT_WAVEFORMATEXTENSIBLE SpeakerHostPinSupportedDeviceFormats[] = {
    LHDC_AUDIO_FORMAT(48000,24,KSDATAFORMAT_SUBTYPE_PCM)
};
#undef LHDC_AUDIO_FORMAT
static MODE_AND_DEFAULT_FORMAT SpeakerHostPinSupportedDeviceModes[] = {
    {STATIC_AUDIO_SIGNALPROCESSINGMODE_DEFAULT,&SpeakerHostPinSupportedDeviceFormats[0].DataFormat},
    {STATIC_AUDIO_SIGNALPROCESSINGMODE_RAW,&SpeakerHostPinSupportedDeviceFormats[0].DataFormat}
};
static PIN_DEVICE_FORMATS_AND_MODES SpeakerPinDeviceFormatsAndModes[] = {
    {SystemRenderPin,SpeakerHostPinSupportedDeviceFormats,SIZEOF_ARRAY(SpeakerHostPinSupportedDeviceFormats),
     SpeakerHostPinSupportedDeviceModes,SIZEOF_ARRAY(SpeakerHostPinSupportedDeviceModes)},
    {BridgePin,nullptr,0,nullptr,0}
};
#define LHDC_AUDIO_RANGE(Rate,Bits,Subtype) \
    {{sizeof(KSDATARANGE_AUDIO),KSDATARANGE_ATTRIBUTES,0,0, \
      STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),{STATIC_##Subtype}, \
      STATICGUIDOF(KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)},2,Bits,Bits,Rate,Rate}
// The supported formats are discrete; a continuous range would also advertise
// sample rates and bit depths that IsFormatSupported correctly rejects.
static KSDATARANGE_AUDIO SpeakerPinDataRangesStream[] = {
    LHDC_AUDIO_RANGE(48000,24,KSDATAFORMAT_SUBTYPE_PCM)
};
#undef LHDC_AUDIO_RANGE
static PKSDATARANGE SpeakerPinDataRangePointersStream[] = {
    PKSDATARANGE(&SpeakerPinDataRangesStream[0]),PKSDATARANGE(&PinDataRangeAttributeList)
};
static KSDATARANGE SpeakerPinDataRangesBridge[] = {
    {sizeof(KSDATARANGE),0,0,0,STATICGUIDOF(KSDATAFORMAT_TYPE_AUDIO),
     STATICGUIDOF(KSDATAFORMAT_SUBTYPE_ANALOG),STATICGUIDOF(KSDATAFORMAT_SPECIFIER_NONE)}
};
static PKSDATARANGE SpeakerPinDataRangePointersBridge[] = {&SpeakerPinDataRangesBridge[0]};
static PCPIN_DESCRIPTOR SpeakerWaveMiniportPins[] = {
    {1,1,0,nullptr,{0,nullptr,0,nullptr,SIZEOF_ARRAY(SpeakerPinDataRangePointersStream),
     SpeakerPinDataRangePointersStream,KSPIN_DATAFLOW_IN,KSPIN_COMMUNICATION_SINK,&KSCATEGORY_AUDIO,nullptr,0}},
    {0,0,0,nullptr,{0,nullptr,0,nullptr,SIZEOF_ARRAY(SpeakerPinDataRangePointersBridge),
     SpeakerPinDataRangePointersBridge,KSPIN_DATAFLOW_OUT,KSPIN_COMMUNICATION_NONE,&KSCATEGORY_AUDIO,nullptr,0}}
};
static PCCONNECTION_DESCRIPTOR SpeakerWaveMiniportConnections[] = {
    {PCFILTER_NODE,KSPIN_WAVE_RENDER3_SINK_SYSTEM,PCFILTER_NODE,KSPIN_WAVE_RENDER3_SOURCE}
};
static PCPROPERTY_ITEM PropertiesSpeakerWaveFilter[] = {
    {&KSPROPSETID_LHDC_PCM,LHDC_PCM_STATUS,KSPROPERTY_TYPE_GET,PropertyHandler_LhdcPcm},
    {&KSPROPSETID_LHDC_PCM,LHDC_PCM_READ,KSPROPERTY_TYPE_GET,PropertyHandler_LhdcPcm},
    {&KSPROPSETID_LHDC_PCM,LHDC_PCM_RESET,KSPROPERTY_TYPE_SET,PropertyHandler_LhdcPcm},
    {&KSPROPSETID_Pin,KSPROPERTY_PIN_PROPOSEDATAFORMAT,
     KSPROPERTY_TYPE_SET|KSPROPERTY_TYPE_BASICSUPPORT,PropertyHandler_WaveFilter},
    {&KSPROPSETID_Pin,KSPROPERTY_PIN_PROPOSEDATAFORMAT2,
     KSPROPERTY_TYPE_GET|KSPROPERTY_TYPE_BASICSUPPORT,PropertyHandler_WaveFilter},
    {&KSPROPSETID_Pin,KSPROPERTY_PIN_MODEDATAFORMATS,
     KSPROPERTY_TYPE_GET|KSPROPERTY_TYPE_BASICSUPPORT,PropertyHandler_LhdcModeFormats}
};
DEFINE_PCAUTOMATION_TABLE_PROP(AutomationSpeakerWaveFilter,PropertiesSpeakerWaveFilter);
static PCFILTER_DESCRIPTOR SpeakerWaveMiniportFilterDescriptor = {
    0,&AutomationSpeakerWaveFilter,sizeof(PCPIN_DESCRIPTOR),SIZEOF_ARRAY(SpeakerWaveMiniportPins),
    SpeakerWaveMiniportPins,sizeof(PCNODE_DESCRIPTOR),0,nullptr,
    SIZEOF_ARRAY(SpeakerWaveMiniportConnections),SpeakerWaveMiniportConnections,0,nullptr
};
