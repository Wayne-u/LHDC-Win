#pragma once
#include <lhdc_pcm.h>
class LhdcPcmBridge {
public:
    LhdcPcmBridge();
    void Configure(const WAVEFORMATEXTENSIBLE* format);
    void SetRunning(bool running);
    void SetProtected(bool protected_content);
    void Push(const BYTE* data,ULONG bytes);
    NTSTATUS Property(PPCPROPERTY_REQUEST request);
private:
    // Retain 200 ms of stereo PCM even at 192 kHz / 24 bit.
    static constexpr ULONG capacity=192000*2*3/5;
    KSPIN_LOCK lock_{};
    LHDC_PCM_STATE state_{};
    ULONG read_{},used_{},limit_{};
    bool protected_{};
    BYTE data_[capacity]{};
};
NTSTATUS PropertyHandler_LhdcPcm(PPCPROPERTY_REQUEST request);
