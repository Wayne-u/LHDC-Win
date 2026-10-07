#pragma once
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <devicetopology.h>
#include <wrl/client.h>
#include <avrt.h>
#include <string>
#include <stdexcept>
#include <sstream>
namespace lhdc {
using Microsoft::WRL::ComPtr;
class AudioError : public std::runtime_error {
public:
    AudioError(HRESULT result,const std::string& message):std::runtime_error(message),hr(result){}
    HRESULT hr;
};
inline void audio_check(HRESULT hr,const char* operation) {
    if (FAILED(hr)) {
        std::ostringstream message;
        message << operation << " failed, HRESULT=0x" << std::hex << static_cast<unsigned long>(hr);
        throw AudioError(hr,message.str());
    }
}
class ComApartment {
public:
    explicit ComApartment(DWORD mode=COINIT_APARTMENTTHREADED) { audio_check(CoInitializeEx(nullptr,mode),"Audio COM initialization"); }
    ~ComApartment() { CoUninitialize(); }
    ComApartment(const ComApartment&)=delete;
};
class AudioHandle {
public:
    explicit AudioHandle(HANDLE value):value_(value) {
        if (!value) throw std::runtime_error("Audio handle creation failed, Win32="+std::to_string(GetLastError()));
    }
    ~AudioHandle() { CloseHandle(value_); }
    AudioHandle(const AudioHandle&)=delete;
    HANDLE get() const { return value_; }
private:
    HANDLE value_;
};
class AudioTask {
public:
    AudioTask() {
        DWORD index=0;
        handle_=AvSetMmThreadCharacteristicsW(L"Audio",&index);
        if (!handle_) throw std::runtime_error("MMCSS Audio registration failed, Win32="+std::to_string(GetLastError()));
    }
    ~AudioTask() { AvRevertMmThreadCharacteristics(handle_); }
    AudioTask(const AudioTask&)=delete;
private:
    HANDLE handle_;
};
inline std::wstring audio_wide(const std::string& text) {
    const int length=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.c_str(),-1,nullptr,0);
    if (!length) throw std::invalid_argument("Audio endpoint ID must be UTF-8");
    std::wstring out(static_cast<std::size_t>(length),L'\0');
    if (!MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,text.c_str(),-1,out.data(),length)) throw std::runtime_error("Audio endpoint ID conversion failed");
    out.pop_back(); return out;
}
inline std::string audio_utf8(const wchar_t* text) {
    const int length=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,-1,nullptr,0,nullptr,nullptr);
    if (!length) throw std::runtime_error("Audio device name conversion failed");
    std::string out(static_cast<std::size_t>(length),'\0');
    if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,-1,out.data(),length,nullptr,nullptr)) throw std::runtime_error("Audio device name conversion failed");
    out.pop_back(); return out;
}
inline ComPtr<IMMDevice> audio_device(const std::string& id) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    audio_check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Audio enumerator creation");
    ComPtr<IMMDevice> device;
    audio_check(enumerator->GetDevice(audio_wide(id).c_str(),&device),"Audio source lookup");
    DWORD state=0; audio_check(device->GetState(&state),"Audio source state query");
    ComPtr<IMMEndpoint> endpoint; audio_check(device.As(&endpoint),"Audio endpoint query");
    EDataFlow flow{}; audio_check(endpoint->GetDataFlow(&flow),"Audio source direction query");
    if (state!=DEVICE_STATE_ACTIVE) throw AudioError(AUDCLNT_E_DEVICE_INVALIDATED,"Audio playback endpoint is no longer active");
    if (flow!=eRender) throw std::invalid_argument("Audio source must be a Windows playback endpoint");
    return device;
}
inline std::wstring audio_adapter_id(IMMDevice* device) {
    ComPtr<IDeviceTopology> topology;
    audio_check(device->Activate(__uuidof(IDeviceTopology),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(topology.GetAddressOf())),"Audio endpoint topology");
    ComPtr<IConnector> connector;audio_check(topology->GetConnector(0,&connector),"Audio endpoint connector");
    LPWSTR id=nullptr;audio_check(connector->GetDeviceIdConnectedTo(&id),"Audio endpoint adapter");
    const std::wstring result(id);CoTaskMemFree(id);return result;
}
inline WAVEFORMATEX audio_pcm(unsigned rate,unsigned bits) {
    WAVEFORMATEX format{};
    format.wFormatTag=WAVE_FORMAT_PCM; format.nChannels=2;
    format.nSamplesPerSec=rate; format.wBitsPerSample=static_cast<WORD>(bits);
    format.nBlockAlign=static_cast<WORD>(2*(bits/8)); format.nAvgBytesPerSec=rate*format.nBlockAlign;
    return format;
}
inline constexpr DWORD audio_convert_flags=AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM|AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
}
