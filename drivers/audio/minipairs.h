// Copyright (c) Microsoft Corporation. All Rights Reserved.
// Derived from Simple Audio Sample, licensed under MS-PL.
// LHDC: one render endpoint, no hardware capture, offload or loopback pin.
#pragma once
#include "speakertopo.h"
#include "speakertoptable.h"
#include "speakerwavtable.h"
NTSTATUS CreateMiniportWaveRTSimpleAudioSample(PUNKNOWN*, REFCLSID, PUNKNOWN, POOL_FLAGS, PUNKNOWN, PVOID, PENDPOINT_MINIPAIR);
NTSTATUS CreateMiniportTopologySimpleAudioSample(PUNKNOWN*, REFCLSID, PUNKNOWN, POOL_FLAGS, PUNKNOWN, PVOID, PENDPOINT_MINIPAIR);
static PHYSICALCONNECTIONTABLE SpeakerTopologyPhysicalConnections[] = {
    {KSPIN_TOPO_WAVEOUT_SOURCE, KSPIN_WAVE_RENDER3_SOURCE, CONNECTIONTYPE_WAVE_OUTPUT}
};
static ENDPOINT_MINIPAIR SpeakerMiniports = {
    eSpeakerDevice, L"TopologySpeaker", nullptr, CreateMiniportTopologySimpleAudioSample,
    &SpeakerTopoMiniportFilterDescriptor, 0, nullptr,
    L"WaveSpeaker", nullptr, CreateMiniportWaveRTSimpleAudioSample,
    &SpeakerWaveMiniportFilterDescriptor, 0, nullptr,
    SPEAKER_DEVICE_MAX_CHANNELS, SpeakerPinDeviceFormatsAndModes,
    SIZEOF_ARRAY(SpeakerPinDeviceFormatsAndModes), SpeakerTopologyPhysicalConnections,
    SIZEOF_ARRAY(SpeakerTopologyPhysicalConnections), ENDPOINT_NO_FLAGS
};
static PENDPOINT_MINIPAIR g_RenderEndpoints[] = {&SpeakerMiniports};
#define g_cRenderEndpoints SIZEOF_ARRAY(g_RenderEndpoints)
#define g_MaxMiniports 2
