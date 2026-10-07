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
    static constexpr ULONG capacity=96000;
    KSPIN_LOCK lock_{};
    LHDC_PCM_STATE state_{};
    ULONG read_{},used_{},limit_{};
    bool protected_{};
    BYTE data_[capacity]{};
};
NTSTATUS PropertyHandler_LhdcPcm(PPCPROPERTY_REQUEST request);
