#include <windows.h>
#include <cguid.h>
#include <initguid.h>
#include "wasapi.hpp"
#include "hfp_activity.hpp"
#include "wav.hpp"
#include "host/json.hpp"
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <setupapi.h>
#include <endpointvolume.h>
#include <audiopolicy.h>
#include <wrl/implements.h>
#include <iostream>
#include <syncstream>
#include <memory>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <vector>
namespace lhdc {
class RenderSessionEvents final : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IAudioSessionEvents> {
public:
    HRESULT STDMETHODCALLTYPE OnDisplayNameChanged(LPCWSTR,LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnIconPathChanged(LPCWSTR,LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSimpleVolumeChanged(float,BOOL,LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnChannelVolumeChanged(DWORD,float*,DWORD,LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnGroupingParamChanged(LPCGUID,LPCGUID) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnStateChanged(AudioSessionState) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSessionDisconnected(AudioSessionDisconnectReason reason) override {
        std::osyncstream(std::cout) << "{\"event\":\"render_session_disconnected\",\"reason\":" << static_cast<unsigned>(reason) << "}" << std::endl;
        return S_OK;
    }
};
class RenderSessionNotifications {
public:
    explicit RenderSessionNotifications(IAudioClient* client) {
        audio_check(client->GetService(IID_PPV_ARGS(&session_)),"Render session control");
        listener_=Microsoft::WRL::Make<RenderSessionEvents>();
        audio_check(session_->RegisterAudioSessionNotification(listener_.Get()),"Render session notification registration");
    }
    ~RenderSessionNotifications() { session_->UnregisterAudioSessionNotification(listener_.Get()); }
private:
    ComPtr<IAudioSessionControl> session_;
    ComPtr<RenderSessionEvents> listener_;
};
using WavePtr=std::unique_ptr<WAVEFORMATEX,decltype(&CoTaskMemFree)>;
static ComPtr<IAudioClient> client_for(IMMDevice* device) {
    ComPtr<IAudioClient> client;
    audio_check(device->Activate(__uuidof(IAudioClient),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(client.GetAddressOf())),"WASAPI client activation");
    return client;
}
static WavePtr mix_format(IAudioClient* client) {
    WAVEFORMATEX* format=nullptr;
    audio_check(client->GetMixFormat(&format),"WASAPI mix format query");
    return WavePtr(format,&CoTaskMemFree);
}
static WAVEFORMATEX device_format(IMMDevice* device) {
    ComPtr<IPropertyStore> store;
    audio_check(device->OpenPropertyStore(STGM_READ,&store),"Audio endpoint properties");
    PROPVARIANT value{};
    audio_check(store->GetValue(PKEY_AudioEngine_DeviceFormat,&value),"Audio endpoint device format");
    if (value.vt!=VT_BLOB || value.blob.cbSize<sizeof(WAVEFORMATEX)) {
        PropVariantClear(&value);
        throw std::runtime_error("Audio endpoint device format is invalid");
    }
    WAVEFORMATEX format{};
    std::memcpy(&format,value.blob.pBlobData,sizeof(format));
    PropVariantClear(&value);
    return format;
}
static std::string device_id(IMMDevice* device) {
    LPWSTR id=nullptr; audio_check(device->GetId(&id),"Audio endpoint ID query");
    std::unique_ptr<wchar_t,decltype(&CoTaskMemFree)> memory(id,&CoTaskMemFree);
    return audio_utf8(id);
}
static std::string device_name(IMMDevice* device) {
    ComPtr<IPropertyStore> store; audio_check(device->OpenPropertyStore(STGM_READ,&store),"Audio endpoint properties");
    PROPVARIANT value{};
    audio_check(store->GetValue(PKEY_Device_FriendlyName,&value),"Audio endpoint friendly name");
    if (value.vt!=VT_LPWSTR) { PropVariantClear(&value); throw std::runtime_error("Audio endpoint has no text name"); }
    const auto name=audio_utf8(value.pwszVal); PropVariantClear(&value); return name;
}
void inspect_audio_devices(bool capture) {
    ComApartment apartment;
    ComPtr<IMMDeviceEnumerator> enumerator;
    audio_check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Audio enumerator creation");
    ComPtr<IMMDevice> default_device;
    const auto flow=capture?eCapture:eRender;
    const auto hr=enumerator->GetDefaultAudioEndpoint(flow,eMultimedia,&default_device);
    if (hr!=HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) audio_check(hr,"Default playback endpoint query");
    const auto default_id=default_device?device_id(default_device.Get()):"";
    ComPtr<IMMDeviceCollection> devices;
    audio_check(enumerator->EnumAudioEndpoints(flow,DEVICE_STATE_ACTIVE,&devices),"Active audio endpoint enumeration");
    UINT count=0; audio_check(devices->GetCount(&count),"Audio endpoint count");
    std::ostringstream out,unavailable; out << "{\"event\":"<<json_string(capture?"audio_inputs":"audio_devices")<<",\"devices\":[";
    bool first=true,first_unavailable=true;
    for (UINT i=0;i<count;++i) {
        ComPtr<IMMDevice> device; audio_check(devices->Item(i,&device),"Audio endpoint selection");
        const auto id=device_id(device.Get());
        try {
        const auto name=device_name(device.Get());
        const auto client=client_for(device.Get());
        const auto format=mix_format(client.Get());
        const auto output_format=device_format(device.Get());
        const auto adapter=audio_utf8(audio_adapter_id(device.Get()).c_str());
        ComPtr<IPropertyStore> store;
        audio_check(device->OpenPropertyStore(STGM_READ,&store),"Audio endpoint identity");
        PROPVARIANT identity{};
        audio_check(store->GetValue(PKEY_Device_ContainerId,&identity),"Audio endpoint container");
        wchar_t container[40]{};
        if(identity.vt==VT_CLSID && identity.puuid) StringFromGUID2(*identity.puuid,container,40);
        PropVariantClear(&identity);
        ComPtr<IAudioEndpointVolume> volume;
        audio_check(device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(volume.GetAddressOf())),"Audio endpoint volume activation");
        float scalar=0,db=0; BOOL muted=FALSE;
        audio_check(volume->GetMasterVolumeLevelScalar(&scalar),"Audio endpoint volume query");
        audio_check(volume->GetMasterVolumeLevel(&db),"Audio endpoint decibel query");
        audio_check(volume->GetMute(&muted),"Audio endpoint mute query");
        if (!first) out << ','; first=false;
        out << "{\"id\":" << json_string(id) << ",\"name\":" << json_string(name) << ",\"default\":" << (id==default_id?"true":"false")
            << ",\"sample_rate\":" << format->nSamplesPerSec << ",\"bits\":" << format->wBitsPerSample << ",\"channels\":" << format->nChannels
            << ",\"device_sample_rate\":" << output_format.nSamplesPerSec << ",\"device_bits\":" << output_format.wBitsPerSample << ",\"adapter_id\":" << json_string(adapter) << ",\"container_id\":"<<json_string(audio_utf8(container))
            << ",\"volume_scalar\":" << scalar << ",\"volume_db\":" << db << ",\"muted\":" << (muted?"true":"false") << '}';
        } catch (const AudioError& error) {
            // Endpoint removal and the audio endpoint builder are asynchronous.
            // Report that exact stale entry while retaining the active devices.
            if (error.hr!=static_cast<HRESULT>(ERROR_NO_SUCH_DEVINST) && error.hr!=HRESULT_FROM_WIN32(ERROR_NOT_FOUND) &&
                error.hr!=AUDCLNT_E_DEVICE_INVALIDATED && error.hr!=AUDCLNT_E_UNSUPPORTED_FORMAT) throw;
            if (!first_unavailable) unavailable << ','; first_unavailable=false;
            unavailable << "{\"id\":" << json_string(id) << ",\"error\":" << json_string(error.what()) << '}';
        }
    }
    out << "],\"unavailable\":[" << unavailable.str() << "]}"; std::cout << out.str() << std::endl;
}
void inspect_playback_ready(const std::string& endpoint,unsigned rate,unsigned bits) {
    ComApartment apartment;
    try {
        const auto device=audio_device(endpoint);
        const auto client=client_for(device.Get());
        auto format=audio_pcm(rate,bits);
        audio_check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_NOPERSIST|audio_convert_flags,0,0,&format,nullptr),"Playback readiness initialization");
        ComPtr<IAudioSessionControl> session;
        audio_check(client->GetService(IID_PPV_ARGS(&session)),"Playback readiness session");
        ComPtr<IAudioRenderClient> writer;
        audio_check(client->GetService(IID_PPV_ARGS(&writer)),"Playback readiness buffers");
        std::cout<<"{\"event\":\"playback_ready\",\"ready\":true,\"started\":false}"<<std::endl;
    } catch(const AudioError& error) {
        if(error.hr!=AUDCLNT_E_DEVICE_INVALIDATED && error.hr!=HRESULT_FROM_WIN32(ERROR_NOT_FOUND) &&
            error.hr!=static_cast<HRESULT>(ERROR_NO_SUCH_DEVINST) && error.hr!=AUDCLNT_E_UNSUPPORTED_FORMAT) throw;
        std::cout<<"{\"event\":\"playback_ready\",\"ready\":false,\"error\":"<<json_string(error.what())<<"}"<<std::endl;
    }
}
class RunningAudio {
public:
    explicit RunningAudio(IAudioClient* client):client_(client){}
    void start() { audio_check(client_->Start(),"WASAPI stream start"); active_=true; }
    ~RunningAudio() { if (active_) client_->Stop(); }
private:
    IAudioClient* client_;
    bool active_=false;
};
void render_wav(const std::string& endpoint,const std::filesystem::path& input,unsigned repeats) {
    if(!repeats || repeats>720) throw std::invalid_argument("WAV repeat count must be 1..720");
    const auto wav=read_wav(input);
    ComApartment apartment; AudioTask priority;
    const auto device=audio_device(endpoint);
    const auto client=client_for(device.Get());
    auto format=audio_pcm(wav.sample_rate,wav.bits);
    audio_check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,AUDCLNT_STREAMFLAGS_EVENTCALLBACK|AUDCLNT_STREAMFLAGS_NOPERSIST|audio_convert_flags,0,0,&format,nullptr),"WASAPI WAV renderer initialization");
    RenderSessionNotifications notifications(client.Get());
    AudioHandle event(CreateEventW(nullptr,FALSE,FALSE,nullptr));
    audio_check(client->SetEventHandle(event.get()),"WAV render event registration");
    ComPtr<IAudioRenderClient> writer; audio_check(client->GetService(IID_PPV_ARGS(&writer)),"WAV render service");
    UINT32 capacity=0; audio_check(client->GetBufferSize(&capacity),"WAV render buffer size");
    const auto source_frames=wav.pcm.size()/format.nBlockAlign;
    const auto total=source_frames*repeats;
    std::size_t submitted=0;
    auto fill=[&] {
        UINT32 padding=0; audio_check(client->GetCurrentPadding(&padding),"WAV render padding query");
        const auto frames=static_cast<UINT32>(std::min<std::size_t>(capacity-padding,total-submitted));
        if (frames) {
            BYTE* data=nullptr; audio_check(writer->GetBuffer(frames,&data),"WAV render buffer");
            std::size_t copied=0;
            while(copied<frames) {
                const auto offset=(submitted+copied)%source_frames;
                const auto count=std::min<std::size_t>(frames-copied,source_frames-offset);
                std::memcpy(data+copied*format.nBlockAlign,wav.pcm.data()+offset*format.nBlockAlign,count*format.nBlockAlign);
                copied+=count;
            }
            audio_check(writer->ReleaseBuffer(frames,0),"WAV render buffer release"); submitted+=frames;
        }
        return padding;
    };
    fill(); RunningAudio playing(client.Get()); playing.start();
    std::cout << "{\"event\":\"wav_render_started\",\"source_id\":" << json_string(endpoint) << "}" << std::endl;
    for (;;) {
        if (WaitForSingleObject(event.get(),2000)!=WAIT_OBJECT_0) throw std::runtime_error("WAV renderer event timeout");
        const auto padding=fill();
        if (submitted==total && !padding) break;
    }
    std::cout << "{\"event\":\"wav_render_complete\",\"frames\":" << submitted << ",\"sample_rate\":" << wav.sample_rate << ",\"bits\":" << wav.bits << "}" << std::endl;
}
void probe_headphone_microphone(unsigned seconds) {
    if(!seconds || seconds>30) throw std::invalid_argument("Microphone probe duration must be 1..30 seconds");
    ComApartment apartment(COINIT_MULTITHREADED);
    const auto device=HfpActivity{}.capture_device();
    if(!device) throw std::runtime_error("The target headphone microphone is unavailable");
    const auto client=client_for(device.Get());
    const auto format=mix_format(client.Get());
    audio_check(client->Initialize(AUDCLNT_SHAREMODE_SHARED,0,2000000,0,format.get(),nullptr),"Microphone probe initialization");
    ComPtr<IAudioCaptureClient> capture;audio_check(client->GetService(IID_PPV_ARGS(&capture)),"Microphone probe buffers");
    RunningAudio running(client.Get());running.start();
    const auto started=std::chrono::steady_clock::now();
    const auto end=started+std::chrono::seconds(seconds);
    auto previous_poll=started;
    std::uint64_t frames=0,position_gaps=0,silent_frames=0,max_poll_us=0;
    UINT64 previous_end=0,first_qpc=0,last_qpc=0;
    UINT32 last_frames=0;
    unsigned packets=0,discontinuities=0,position_regressions=0,timestamp_errors=0;
    bool have_position=false;
    struct Packet { unsigned index; UINT32 frames; DWORD flags; UINT64 position,qpc; std::int64_t delta; };
    std::vector<Packet> anomalies;
    anomalies.reserve(seconds*100);
    std::cout<<"{\"event\":\"microphone_open\",\"rate\":"<<format->nSamplesPerSec<<",\"channels\":"<<format->nChannels<<"}"<<std::endl;
    while(std::chrono::steady_clock::now()<end) {
        const auto now=std::chrono::steady_clock::now();
        max_poll_us=std::max(max_poll_us,static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now-previous_poll).count()));
        previous_poll=now;
        UINT32 count=0;audio_check(capture->GetNextPacketSize(&count),"Microphone probe packet");
        while(count) {
            BYTE* data=nullptr;DWORD flags=0;UINT64 position=0,qpc=0;
            audio_check(capture->GetBuffer(&data,&count,&flags,&position,&qpc),"Microphone probe read");
            if(!count) break;
            frames+=count;if(flags&AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) ++discontinuities;
            if(flags&AUDCLNT_BUFFERFLAGS_SILENT) silent_frames+=count;
            const auto delta=have_position ? static_cast<std::int64_t>(position)-static_cast<std::int64_t>(previous_end) : 0;
            if(flags&AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) { ++timestamp_errors;have_position=false; }
            else {
                if(have_position && delta>0) position_gaps+=static_cast<std::uint64_t>(delta);
                if(have_position && delta<0) ++position_regressions;
                previous_end=position+count;have_position=true;
                if(!first_qpc) first_qpc=qpc;
                last_qpc=qpc;last_frames=count;
            }
            ++packets;
            if((flags&(AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY|AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)) || delta)
                anomalies.push_back({packets,count,flags,position,qpc,delta});
            audio_check(capture->ReleaseBuffer(count),"Microphone probe discard");
            audio_check(capture->GetNextPacketSize(&count),"Microphone probe next packet");
        }
        Sleep(5);
    }
    for(const auto& packet:anomalies)
        std::cout<<"{\"event\":\"microphone_packet\",\"index\":"<<packet.index<<",\"frames\":"<<packet.frames<<",\"flags\":"<<packet.flags
                 <<",\"position\":"<<packet.position<<",\"qpc_100ns\":"<<packet.qpc<<",\"position_delta\":"<<packet.delta<<"}"<<'\n';
    const auto timestamp_span_ms=last_qpc>=first_qpc && first_qpc ? (last_qpc-first_qpc)/10000.0+last_frames*1000.0/format->nSamplesPerSec : 0;
    std::cout<<"{\"event\":\"microphone_complete\",\"frames\":"<<frames<<",\"packets\":"<<packets<<",\"discontinuities\":"<<discontinuities
             <<",\"position_gap_frames\":"<<position_gaps<<",\"position_regressions\":"<<position_regressions<<",\"timestamp_errors\":"<<timestamp_errors
             <<",\"silent_frames\":"<<silent_frames<<",\"max_poll_us\":"<<max_poll_us<<",\"timestamp_span_ms\":"<<timestamp_span_ms<<",\"recorded\":false}"<<std::endl;
    if(!frames) throw std::runtime_error("The headphone microphone produced no audio frames");
}
}
