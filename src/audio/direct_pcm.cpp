#include "direct_pcm.hpp"
#include "transport/windows.hpp"
#include "host/json.hpp"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <chrono>
#include <thread>
#include <fstream>
#include <sstream>
#include <cmath>
namespace lhdc {
namespace {
[[noreturn]] void pcm_error(const char* operation) { throw WindowsError(operation,GetLastError()); }
void validate(const LHDC_PCM_STATE& state) {
    static_assert(sizeof(LHDC_PCM_STATE)==64);
    if (state.Size!=sizeof(state) || state.Version!=LHDC_PCM_ABI_VERSION)
        throw std::runtime_error("Direct PCM ABI differs from the installed audio driver");
}
struct DeviceSet {
    HDEVINFO value=SetupDiGetClassDevsW(&KSCATEGORY_AUDIO,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE);
    DeviceSet() { if (value==INVALID_HANDLE_VALUE) pcm_error("Audio driver interface enumeration"); }
    ~DeviceSet() { SetupDiDestroyDeviceInfoList(value); }
};
}
std::optional<std::wstring> find_direct_pcm() {
    DeviceSet set;
    std::optional<std::wstring> found;
    for (DWORD i=0;;++i) {
        SP_DEVICE_INTERFACE_DATA item{sizeof(item)};
        if (!SetupDiEnumDeviceInterfaces(set.value,nullptr,&KSCATEGORY_AUDIO,i,&item)) {
            if (GetLastError()==ERROR_NO_MORE_ITEMS) break;
            pcm_error("Audio driver interface");
        }
        DWORD size=0;
        if (SetupDiGetDeviceInterfaceDetailW(set.value,&item,nullptr,0,&size,nullptr) || GetLastError()!=ERROR_INSUFFICIENT_BUFFER)
            pcm_error("Audio driver interface detail size");
        std::vector<BYTE> bytes(size);
        auto* detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(bytes.data()); detail->cbSize=sizeof(*detail);
        SP_DEVINFO_DATA device{sizeof(device)};
        if (!SetupDiGetDeviceInterfaceDetailW(set.value,&item,detail,size,nullptr,&device)) pcm_error("Audio driver interface detail");
        std::array<wchar_t,MAX_DEVICE_ID_LEN> instance{};
        if (!SetupDiGetDeviceInstanceIdW(set.value,&device,instance.data(),static_cast<DWORD>(instance.size()),nullptr)) pcm_error("Audio child instance");
        if (_wcsnicmp(instance.data(),L"LHDCWIN\\AUDIO\\",14)!=0) continue;
        DEVINST parent=0;
        if (CM_Get_Parent(&parent,device.DevInst,0)!=CR_SUCCESS || CM_Get_Device_IDW(parent,instance.data(),static_cast<ULONG>(instance.size()),0)!=CR_SUCCESS)
            throw std::runtime_error("Audio child parent lookup failed");
        if (_wcsicmp(instance.data(),target_device().instance.c_str())!=0) continue;
        const std::wstring path=detail->DevicePath;
        constexpr wchar_t suffix[]=L"\\WaveSpeaker";
        if (path.size()<std::size(suffix)-1 || _wcsicmp(path.c_str()+path.size()-(std::size(suffix)-1),suffix)!=0) continue;
        if (found) throw std::runtime_error("More than one PCM interface belongs to the target headphones");
        found=path;
    }
    return found;
}
DirectPcm::DirectPcm(const std::wstring& path) {
    handle_=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    if (handle_==INVALID_HANDLE_VALUE) pcm_error("Direct PCM filter open");
}
DirectPcm::~DirectPcm() { CloseHandle(handle_); }
DWORD DirectPcm::property(ULONG id,bool set,void* data,DWORD size) {
    KSPROPERTY request{KSPROPSETID_LHDC_PCM,id,static_cast<ULONG>(set?KSPROPERTY_TYPE_SET:KSPROPERTY_TYPE_GET)};
    DWORD returned=0;
    if (!DeviceIoControl(handle_,IOCTL_KS_PROPERTY,&request,sizeof(request),data,size,&returned,nullptr)) pcm_error("Direct PCM property");
    return returned;
}
LHDC_PCM_STATE DirectPcm::state() {
    LHDC_PCM_STATE state{};
    if (property(LHDC_PCM_STATUS,false,&state,sizeof(state))!=sizeof(state)) throw std::runtime_error("Invalid direct PCM state size");
    validate(state); return state;
}
void DirectPcm::reset() { ULONG version=LHDC_PCM_ABI_VERSION; property(LHDC_PCM_RESET,true,&version,sizeof(version)); }
void DirectPcm::inspect_formats() {
    std::ostringstream output;
    struct Request { KSP_PIN pin; GUID mode; };
    const GUID modes[]={AUDIO_SIGNALPROCESSINGMODE_DEFAULT,AUDIO_SIGNALPROCESSINGMODE_RAW};
    const char* names[]={"default","raw"};
    output << "{\"event\":\"direct_formats\",\"modes\":[";
    for (unsigned index=0;index<std::size(modes);++index) {
        Request request{};
        request.pin.Property={KSPROPSETID_Pin,KSPROPERTY_PIN_MODEDATAFORMATS,KSPROPERTY_TYPE_GET};
        request.pin.PinId=0;
        request.mode=modes[index];
        DWORD size=0;
        if (DeviceIoControl(handle_,IOCTL_KS_PROPERTY,&request,sizeof(request),nullptr,0,&size,nullptr) ||
            (GetLastError()!=ERROR_MORE_DATA && GetLastError()!=ERROR_INSUFFICIENT_BUFFER)) pcm_error("Mode format list size query");
        if (size<sizeof(KSMULTIPLE_ITEM) || size>65536) throw std::runtime_error("Invalid mode format list size");
        std::vector<BYTE> bytes(size);
        DWORD returned=0;
        if (!DeviceIoControl(handle_,IOCTL_KS_PROPERTY,&request,sizeof(request),bytes.data(),size,&returned,nullptr)) pcm_error("Mode format list query");
        if (returned<sizeof(KSMULTIPLE_ITEM)) throw std::runtime_error("Short mode format list");
        const auto* items=reinterpret_cast<const KSMULTIPLE_ITEM*>(bytes.data());
        if (items->Size!=returned || items->Count>(returned-sizeof(*items))/sizeof(ULONG))
            throw std::runtime_error("Invalid mode format list header");
        const auto* offsets=reinterpret_cast<const ULONG*>(items+1);
        if (index) output << ',';
        output << "{\"mode\":\"" << names[index] << "\",\"formats\":[";
        for (ULONG format_index=0;format_index<items->Count;++format_index) {
            const auto offset=sizeof(*items)+static_cast<std::size_t>(offsets[format_index]);
            const auto header_bytes=sizeof(*items)+items->Count*sizeof(ULONG);
            if (offset<header_bytes || offset%8 || offset>returned || returned-offset<sizeof(KSDATAFORMAT_WAVEFORMATEXTENSIBLE))
                throw std::runtime_error("Invalid mode format list offset");
            const auto* format=reinterpret_cast<const KSDATAFORMAT_WAVEFORMATEXTENSIBLE*>(bytes.data()+offset);
            if (format->DataFormat.FormatSize!=sizeof(*format) || format->WaveFormatExt.Format.wFormatTag!=WAVE_FORMAT_EXTENSIBLE)
                throw std::runtime_error("Unexpected mode format descriptor");
            const auto& wave=format->WaveFormatExt;
            if (format_index) output << ',';
            output << "{\"sample_rate\":" << wave.Format.nSamplesPerSec << ",\"bits\":" << wave.Samples.wValidBitsPerSample
                << ",\"container_bits\":" << wave.Format.wBitsPerSample << ",\"channels\":" << wave.Format.nChannels << '}';
        }
        output << "]}";
    }
    output << "]}\n";
    std::cout << output.str();
}
void inspect_direct_formats() {
    const auto path=find_direct_pcm();
    if (!path) throw std::runtime_error("LHDC audio interface is unavailable");
    DirectPcm pcm(*path);
    pcm.inspect_formats();
}
DirectPcmChunk DirectPcm::read(std::size_t max_bytes) {
    if (!max_bytes || max_bytes>16384) throw std::invalid_argument("Direct PCM read size must be 1..16384 bytes");
    std::array<std::uint8_t,sizeof(LHDC_PCM_STATE)+16384> buffer{};
    const auto returned=property(LHDC_PCM_READ,false,buffer.data(),static_cast<DWORD>(sizeof(LHDC_PCM_STATE)+max_bytes));
    if (returned<sizeof(LHDC_PCM_STATE)) throw std::runtime_error("Short direct PCM response");
    DirectPcmChunk chunk{}; std::memcpy(&chunk.state,buffer.data(),sizeof(chunk.state)); validate(chunk.state);
    if (chunk.state.PayloadBytes!=returned-sizeof(chunk.state) || (chunk.state.PayloadBytes && (!chunk.state.BlockAlign || chunk.state.PayloadBytes%chunk.state.BlockAlign)))
        throw std::runtime_error("Invalid direct PCM payload alignment");
    chunk.bytes.assign(buffer.begin()+sizeof(chunk.state),buffer.begin()+returned);
    return chunk;
}
void inspect_direct_pcm() {
    const auto path=find_direct_pcm();
    if (!path) { std::cout << "{\"event\":\"direct_pcm\",\"present\":false}\n"; return; }
    DirectPcm pcm(*path); const auto s=pcm.state();
    std::cout << "{\"event\":\"direct_pcm\",\"present\":true,\"abi\":" << s.Version << ",\"running\":" << (s.Running?"true":"false")
        << ",\"epoch\":" << s.Epoch << ",\"sample_rate\":" << s.SampleRate << ",\"bits\":" << s.Bits << ",\"channels\":" << s.Channels
        << ",\"container_bits\":" << (s.Channels?s.BlockAlign*8/s.Channels:0)
        << ",\"floating_point\":" << (s.FloatingPoint?"true":"false") << ",\"buffered_bytes\":" << s.BufferedBytes << ",\"dropped_bytes\":" << s.DroppedBytes << "}\n";
}
void capture_direct_pcm(unsigned seconds,const std::filesystem::path& output) {
    if (!seconds || seconds>60) throw std::invalid_argument("Direct capture duration must be 1..60 seconds");
    const auto path=find_direct_pcm();
    if (!path) throw std::runtime_error("The headphone PCM interface is absent");
    DirectPcm pcm(*path); const auto format=pcm.state();
    if (!format.Running || format.Channels!=2) throw std::runtime_error("Start playback on the headphone endpoint before direct capture");
    pcm.reset();
    std::ofstream file(output,std::ios::binary); file.exceptions(std::ios::failbit|std::ios::badbit);
    const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(seconds);
    std::uint64_t bytes=0,nonzero=0; double peak=0;
    while (std::chrono::steady_clock::now()<end) {
        const auto chunk=pcm.read();
        if (chunk.state.Epoch!=format.Epoch || !chunk.state.Running || chunk.state.DroppedBytes)
            throw std::runtime_error("Direct capture stream changed, stopped or overran");
        if (!chunk.bytes.empty()) {
            file.write(reinterpret_cast<const char*>(chunk.bytes.data()),static_cast<std::streamsize>(chunk.bytes.size()));
            bytes+=chunk.bytes.size();
            for (std::size_t offset=0;offset<chunk.bytes.size();offset+=format.Bits/8) {
                const auto* data=chunk.bytes.data()+offset;
                double value=0;
                if (format.FloatingPoint && format.Bits==32) { float sample; std::memcpy(&sample,data,4); value=sample; }
                else if (format.Bits==16) { value=static_cast<std::int16_t>(data[0]|(data[1]<<8))/32768.0; }
                else if (format.Bits==24) {
                    std::int32_t sample=data[0]|(data[1]<<8)|(data[2]<<16); if (sample&0x800000) sample-=0x1000000;
                    value=sample/8388608.0;
                } else throw std::runtime_error("Unsupported direct capture sample representation");
                peak=std::max(peak,std::abs(value)); if (value!=0) ++nonzero;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    std::cout << "{\"event\":\"direct_capture_complete\",\"source\":\"WaveRT\",\"sample_rate\":" << format.SampleRate << ",\"bits\":" << format.Bits
        << ",\"channels\":" << format.Channels << ",\"floating_point\":" << (format.FloatingPoint?"true":"false") << ",\"frames\":" << bytes/format.BlockAlign
        << ",\"bytes\":" << bytes << ",\"nonzero_samples\":" << nonzero << ",\"peak\":" << peak << ",\"dropped_bytes\":0}\n";
}
}
