#include "volume_sync.hpp"
#include "windows_audio.hpp"
#include "avrcp/protocol.hpp"
#include "host/json.hpp"
#include "volume_memory.hpp"
#include <endpointvolume.h>
#include <wrl/implements.h>
#include <future>
#include <iostream>
#include <syncstream>
#include <optional>
#include <cmath>
#include <iomanip>
#include <sstream>
namespace lhdc {
namespace {
namespace av=avrcp;
void send_media_command(av::MediaCommand command) {
    unsigned code=0;
    switch (command) {
    case av::MediaCommand::play: code=APPCOMMAND_MEDIA_PLAY;break;
    case av::MediaCommand::pause: code=APPCOMMAND_MEDIA_PAUSE;break;
    case av::MediaCommand::stop: code=APPCOMMAND_MEDIA_STOP;break;
    case av::MediaCommand::next: code=APPCOMMAND_MEDIA_NEXTTRACK;break;
    case av::MediaCommand::previous: code=APPCOMMAND_MEDIA_PREVIOUSTRACK;break;
    case av::MediaCommand::rewind: code=APPCOMMAND_MEDIA_REWIND;break;
    case av::MediaCommand::fast_forward: code=APPCOMMAND_MEDIA_FAST_FORWARD;break;
    }
    const auto window=GetForegroundWindow();
    if (!window) throw WindowsError("Media command target",ERROR_INVALID_WINDOW_HANDLE);
    DWORD_PTR result=0;
    if (!SendMessageTimeoutW(window,WM_APPCOMMAND,reinterpret_cast<WPARAM>(window),
        static_cast<LPARAM>(code|FAPPCOMMAND_OEM)<<16,SMTO_ABORTIFHUNG,250,&result))
        throw WindowsError("Media command dispatch",GetLastError());
}
constexpr GUID volume_context={0xd9c90dc2,0x245d,0x47ac,{0x9c,0x70,0x31,0xcf,0x1a,0x15,0x30,0xb6}};
std::wstring volume_key() {
    std::wostringstream key;key<<L"SOFTWARE\\LHDC-Win\\Volume\\"<<std::uppercase<<std::hex<<std::setfill(L'0')<<std::setw(12)<<target_device().address;
    return key.str();
}
std::optional<VolumeMemory> saved_volume() {
    VolumeMemory value{};DWORD size=sizeof(value);
    const auto status=RegGetValueW(HKEY_LOCAL_MACHINE,volume_key().c_str(),L"State",RRF_RT_REG_BINARY,nullptr,&value,&size);
    if (status==ERROR_FILE_NOT_FOUND) return {};
    if (status!=ERROR_SUCCESS) throw WindowsError("Volume memory read",status);
    if (size!=sizeof(value) || !valid_volume_memory(value)) throw std::runtime_error("Invalid saved headphone volume");
    return value;
}
void save_volume(const VolumeMemory& value) {
    HKEY key=nullptr;
    auto status=RegCreateKeyExW(HKEY_LOCAL_MACHINE,volume_key().c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    if (status!=ERROR_SUCCESS) throw WindowsError("Volume memory key",status);
    status=RegSetValueExW(key,L"State",0,REG_BINARY,reinterpret_cast<const BYTE*>(&value),sizeof(value));
    RegCloseKey(key);
    if (status!=ERROR_SUCCESS) throw WindowsError("Volume memory write",status);
}
ComPtr<IAudioEndpointVolume> headphone_volume(bool absolute) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    audio_check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Volume enumerator");
    ComPtr<IMMDeviceCollection> devices;
    audio_check(enumerator->EnumAudioEndpoints(eRender,DEVICE_STATE_ACTIVE,&devices),"Volume endpoints");
    UINT count=0;audio_check(devices->GetCount(&count),"Volume endpoint count");
    ComPtr<IAudioEndpointVolume> found;
    for(UINT index=0;index<count;++index) {
        ComPtr<IMMDevice> device;audio_check(devices->Item(index,&device),"Volume endpoint");
        auto adapter=audio_adapter_id(device.Get());
        std::transform(adapter.begin(),adapter.end(),adapter.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});
        if(adapter.find(L"lhdcwin#audio#")==std::wstring::npos) continue;
        if(found) throw std::runtime_error("Multiple LHDC headphone volume endpoints");
        audio_check(device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(found.GetAddressOf())),"Headphone hardware volume");
    }
    if(!found) throw std::runtime_error("LHDC headphone volume endpoint is unavailable");
    DWORD support=0;audio_check(found->QueryHardwareSupport(&support),"Headphone volume hardware support");
    if ((support&3)!=(absolute?3u:0u))
        throw std::runtime_error("Installed audio topology differs from the selected volume mode; reapply the audio driver");
    return found;
}
class VolumeEvents final:public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IAudioEndpointVolumeCallback> {
public:
    // One atomic snapshot binds the requested value to its change sequence.
    std::atomic<std::uint64_t> state=0;
    HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
        if(!data) return E_POINTER;
        if(IsEqualGUID(data->guidEventContext,volume_context)) return S_OK;
        // Keep the pre-mute scalar separate from mute in both gain modes.
        if(!std::isfinite(data->fMasterVolume) || data->fMasterVolume<0 || data->fMasterVolume>1) return E_INVALIDARG;
        const auto value=av::absolute_volume(data->fMasterVolume)|(data->bMuted?128:0);
        auto previous=state.load();
        while(!state.compare_exchange_weak(previous,((previous>>8)+1)*256+value)) {}
        return S_OK;
    }
};
struct VolumeNotifications {
    ComPtr<IAudioEndpointVolume> endpoint;
    ComPtr<VolumeEvents> events=Microsoft::WRL::Make<VolumeEvents>();
    bool restored=false;
    bool absolute;
    std::optional<std::uint64_t> remembered;
    explicit VolumeNotifications(bool use_absolute):endpoint(headphone_volume(use_absolute)),absolute(use_absolute) {
        audio_check(endpoint->RegisterControlChangeNotify(events.Get()),"Volume notification registration");
        float scalar=0;BOOL muted=FALSE;
        audio_check(endpoint->GetMasterVolumeLevelScalar(&scalar),"Initial Windows volume");
        audio_check(endpoint->GetMute(&muted),"Initial Windows mute");
        std::uint64_t initial=0;
        events->state.compare_exchange_strong(initial,av::absolute_volume(scalar)|(muted?128:0));
        const auto saved=saved_volume();
        initial=events->state.load();
        if (saved && restore_volume_memory(*saved,absolute,initial>>8!=0) &&
            events->state.compare_exchange_strong(initial,av::absolute_volume(saved->scalar)|(saved->muted?128:0))) {
            audio_check(endpoint->SetMasterVolumeLevelScalar(saved->scalar,&volume_context),"Remembered headphone volume");
            audio_check(endpoint->SetMute(saved->muted!=0,&volume_context),"Remembered headphone mute");
            restored=true;
        }
    }
    ~VolumeNotifications() { endpoint->UnregisterControlChangeNotify(events.Get()); }
    void remote(std::uint8_t value) {
        audio_check(endpoint->SetMasterVolumeLevelScalar(value/127.0f,&volume_context),"Remote headphone volume feedback");
        audio_check(endpoint->SetMute(FALSE,&volume_context),"Remote headphone mute feedback");
    }
    void remember(std::uint64_t snapshot) {
        if (remembered==snapshot) return;
        VolumeMemory value;value.absolute=absolute;BOOL muted=FALSE;
        audio_check(endpoint->GetMasterVolumeLevelScalar(&value.scalar),"Volume memory scalar");
        audio_check(endpoint->GetMute(&muted),"Volume memory mute");value.muted=muted!=FALSE;
        if (events->state.load()!=snapshot) return;
        save_volume(value);remembered=snapshot;
    }
};
class Controller {
public:
    explicit Controller(Transport& transport):transport_(transport) { transport_.open(LHDC_CHANNEL_AVRCP); }
    ~Controller() {
        try { transport_.close(LHDC_CHANNEL_AVRCP); }
        catch(const std::exception& error) { std::osyncstream(std::cerr)<<"{\"event\":\"avrcp_close_error\",\"message\":"<<json_string(error.what())<<"}"<<std::endl; }
    }
    void send(const av::Bytes& bytes) { transport_.send(bytes,LHDC_CHANNEL_AVRCP); }
    std::optional<av::Frame> receive(DWORD timeout) {
        try {
            auto frame=av::parse(transport_.receive(LHDC_CHANNEL_AVRCP,timeout));
            if(!frame.response) { answer(frame);return {}; }
            return frame;
        } catch(const WindowsError& error) {
            if(error.code==ERROR_TIMEOUT || error.code==ERROR_SEM_TIMEOUT) return {};
            throw;
        }
    }
    av::Frame wait(std::uint8_t label,std::uint8_t pdu) {
        const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(std::chrono::steady_clock::now()<end) {
            const auto frame=receive(50);
            if(frame && frame->label==label && av::vendor(*frame).pdu==pdu) return *frame;
        }
        throw std::runtime_error("AVRCP volume acknowledgement timeout");
    }
private:
    Transport& transport_;
    void answer(const av::Frame& frame) {
        if (const auto button=av::media_button(frame)) {
            if (button->pressed) {
                try { send_media_command(button->command); }
                catch (const std::exception& error) {
                    std::osyncstream(std::cerr)<<"{\"event\":\"avrcp_media_command_error\",\"message\":"<<json_string(error.what())<<"}"<<std::endl;
                    send(av::reply(frame,0x0a,frame.operands));return;
                }
            }
            send(av::reply(frame,av::accepted,frame.operands));return;
        }
        if(frame.opcode==0) {
            const auto pdu=av::vendor(frame);
            if(pdu.pdu==av::get_capabilities && frame.ctype==1 && pdu.parameters.size()==1) {
                // We control remote amplification; we do not expose a local amplifier.
                if(pdu.parameters[0]==3) { const std::uint8_t data[]={0,0x19,0x58,0x10,0,0,2,3,0};send(av::reply(frame,av::stable,data));return; }
                if(pdu.parameters[0]==2) { const std::uint8_t data[]={0,0x19,0x58,0x10,0,0,5,2,1,0,0x19,0x58};send(av::reply(frame,av::stable,data));return; }
            }
        }
        send(av::reply(frame,0x08,frame.operands));
    }
};
void synchronize(Transport& transport,std::stop_token stop,std::promise<void>& ready,bool& announced) {
    const auto hr=CoInitializeEx(nullptr,COINIT_MULTITHREADED);audio_check(hr,"AVRCP COM initialization");
    struct Apartment { ~Apartment(){CoUninitialize();} } apartment;
    VolumeNotifications volume(true);
    Controller controller(transport);
    controller.send(av::capabilities(1));
    if(!av::supports_volume(controller.wait(1,av::get_capabilities))) throw std::runtime_error("Headphones do not support AVRCP volume notifications");
    controller.send(av::notification(2));
    const auto initial=av::volume_value(controller.wait(2,av::register_notification));
    auto initial_state=volume.events->state.load();
    // Recreated endpoints default to 100%. Start from the actual headphone gain,
    // unless Windows is muted or the user changed its slider during negotiation.
    if(!volume.restored && initial_state<128 && volume.events->state.compare_exchange_strong(initial_state,initial)) {
        volume.remote(initial);initial_state=initial;
    }
    const auto requested=static_cast<std::uint8_t>((initial_state&128)?0:initial_state&127);
    // Re-sending the current gain makes some peers quantize it upward again.
    auto confirmed=initial;
    if(requested!=initial) {
        controller.send(av::volume(3,requested));
        confirmed=av::volume_value(controller.wait(3,av::set_absolute_volume));
    }
    if(requested==0 && confirmed!=0) throw std::runtime_error("Headphones did not acknowledge mute");
    // No PCM is sent until the peer acknowledges the initial Windows level.
    if(confirmed!=requested && volume.events->state.compare_exchange_strong(initial_state,(initial_state&~255ull)|confirmed)) volume.remote(confirmed);
    std::osyncstream(std::cout)<<"{\"event\":\"avrcp_volume_ready\",\"previous_remote\":"<<unsigned(initial)<<",\"requested\":"<<unsigned(requested)<<",\"confirmed\":"<<unsigned(confirmed)<<",\"pcm_master_gain\":1}"<<std::endl;
    volume.remember(volume.events->state.load());
    ready.set_value();
    announced=true;
    std::uint8_t label=4;
    bool pending=false;
    unsigned requested_pending=0;
    std::uint64_t revision_pending=0;
    auto deadline=std::chrono::steady_clock::now();
    while(!stop.stop_requested()) {
        const auto snapshot=volume.events->state.load();
        const auto desired=static_cast<unsigned>((snapshot&128)?0:snapshot&127);
        if(!pending && desired!=confirmed) {
            revision_pending=snapshot;requested_pending=desired;
            controller.send(av::volume(label,static_cast<std::uint8_t>(desired)));
            deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);pending=true;
        }
        const auto frame=controller.receive(50);
        if(frame) {
            const auto pdu=av::vendor(*frame);
            if(pending && frame->label==label && pdu.pdu==av::set_absolute_volume) {
                confirmed=av::volume_value(*frame);pending=false;
                if(requested_pending==0 && confirmed!=0) throw std::runtime_error("Headphones did not acknowledge mute");
                if(confirmed!=requested_pending && volume.events->state.compare_exchange_strong(revision_pending,(revision_pending&~255ull)|confirmed)) {
                    volume.remote(confirmed);
                }
                label=static_cast<std::uint8_t>(label==15?4:label+1);
                std::osyncstream(std::cout)<<"{\"event\":\"avrcp_volume_ack\",\"volume\":"<<unsigned(confirmed)<<"}"<<std::endl;
            } else if(frame->label==2 && pdu.pdu==av::register_notification) {
                const auto remote=av::volume_value(*frame);
                if(frame->ctype==av::changed) {
                    // A local Windows change already in flight takes precedence.
                    auto current=volume.events->state.load();
                    const auto desired_now=(current&128)?0:current&127;
                    if(!pending && desired_now==confirmed && volume.events->state.compare_exchange_strong(current,(current&~255ull)|remote)) {volume.remote(remote);confirmed=remote;}
                    controller.send(av::notification(2));
                    std::osyncstream(std::cout)<<"{\"event\":\"avrcp_volume_remote\",\"volume\":"<<unsigned(remote)<<"}"<<std::endl;
                }
            }
        }
        if(pending && std::chrono::steady_clock::now()>deadline) throw std::runtime_error("AVRCP volume change was not acknowledged");
        volume.remember(volume.events->state.load());
    }
    volume.remember(volume.events->state.load());
}
void software_volume(std::stop_token stop,std::promise<void>& ready,bool& announced) {
    ComApartment apartment(COINIT_MULTITHREADED);
    VolumeNotifications volume(false);
    volume.remember(volume.events->state.load());
    ready.set_value();announced=true;
    while (!stop.stop_requested()) {
        volume.remember(volume.events->state.load());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    volume.remember(volume.events->state.load());
}
}
VolumeSync::VolumeSync(Transport& transport,bool absolute) {
    std::promise<void> ready;auto future=ready.get_future();
    worker_=std::jthread([this,&transport,absolute,ready=std::move(ready)](std::stop_token stop) mutable {
        bool announced=false;
        try {
            if (absolute) synchronize(transport,stop,ready,announced);
            else software_volume(stop,ready,announced);
        }
        catch(...) {
            error_=std::current_exception();failed_.store(true);
            if(!announced) ready.set_exception(error_);
        }
    });
    future.get();
}
VolumeSync::~VolumeSync() {worker_.request_stop();worker_.join();}
void VolumeSync::check() const {if(failed_.load()) std::rethrow_exception(error_);}
void inspect_absolute_volume() {
    std::string reason;
    std::optional<unsigned> initial;
    for (unsigned attempt=0;attempt<3;++attempt) {
        try {
            Transport transport(target_device().address);
            Controller controller(transport);
            controller.send(av::capabilities(1));
            if (av::supports_volume(controller.wait(1,av::get_capabilities))) {
                controller.send(av::notification(2));
                initial=av::volume_value(controller.wait(2,av::register_notification));
            } else reason="Peer does not advertise volume notifications";
            break;
        } catch (const WindowsError& error) {
            reason=error.what();
            // Native AVCTP teardown can lag behind PnP's disabled state.
            if ((error.code!=ERROR_REQ_NOT_ACCEP && error.code!=ERROR_BUSY) || attempt==2) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        } catch (const std::exception& error) { reason=error.what();break; }
    }
    std::cout<<"{\"event\":\"absolute_volume_probe\",\"supported\":"<<(initial?"true":"false")
        <<",\"reason\":"<<json_string(reason);
    if (initial) std::cout<<",\"remote_volume\":"<<*initial;
    std::cout<<"}"<<std::endl;
}
}
