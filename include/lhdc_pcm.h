#pragma once
/* Private KS property ABI. PCM originates in the WaveRT render buffer. */
#define LHDC_PCM_ABI_VERSION 1u
#define LHDC_AUDIO_HARDWARE_ID L"LHDCWIN\\Audio"
static const GUID KSPROPSETID_LHDC_PCM =
    {0x441b3045,0xb96a,0x48e9,{0xa2,0xf8,0xad,0x88,0x06,0xa5,0x28,0xd7}};
enum { LHDC_PCM_STATUS=0, LHDC_PCM_READ=1, LHDC_PCM_RESET=2 };
typedef struct LHDC_PCM_STATE {
    ULONG Size;
    ULONG Version;
    ULONGLONG Epoch;
    ULONG SampleRate;
    ULONG Bits;
    ULONG Channels;
    ULONG BlockAlign;
    ULONG FloatingPoint;
    ULONG Running;
    ULONG BufferedBytes;
    ULONG PayloadBytes;
    ULONGLONG ProducedBytes;
    ULONGLONG DroppedBytes;
} LHDC_PCM_STATE;
