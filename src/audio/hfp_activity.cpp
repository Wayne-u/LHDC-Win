#include "hfp_activity.hpp"
#include "direct_pcm.hpp"
#include <audiopolicy.h>
#include <wrl/implements.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>
#include <iostream>
#include <map>
#include <mutex>
#include <vector>
namespace lhdc {
namespace {
class SessionEvents final:public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,IAudioSessionNotification> {
public:
    HRESULT STDMETHODCALLTYPE OnSessionCreated(IAudioSessionControl* session) override {
        std::lock_guard lock(mutex_);sessions_.emplace_back(session);return S_OK;
    }
    bool active() {
        std::lock_guard lock(mutex_);
        bool found=false;
        for(auto it=sessions_.begin();it!=sessions_.end();) {
            AudioSessionState state{};audio_check((*it)->GetState(&state),"HFP session state");
            if(state==AudioSessionStateExpired) { it=sessions_.erase(it);continue; }
            if(state==AudioSessionStateActive) {
                ComPtr<IAudioSessionControl2> details;audio_check(it->As(&details),"HFP session identity");
                DWORD process=0;audio_check(details->GetProcessId(&process),"HFP session process");
                if(process!=GetCurrentProcessId()) found=true;
            }
            ++it;
        }
        return found;
    }
private:
    std::mutex mutex_;
    std::vector<ComPtr<IAudioSessionControl>> sessions_;
};
struct SessionWatch {
    ComPtr<IAudioSessionManager2> manager;
    ComPtr<SessionEvents> events=Microsoft::WRL::Make<SessionEvents>();
    explicit SessionWatch(IMMDevice* device) {
        audio_check(device->Activate(__uuidof(IAudioSessionManager2),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(manager.GetAddressOf())),"HFP session manager");
        audio_check(manager->RegisterSessionNotification(events.Get()),"HFP session registration");
        ComPtr<IAudioSessionEnumerator> sessions;audio_check(manager->GetSessionEnumerator(&sessions),"HFP initial sessions");
        int count=0;audio_check(sessions->GetCount(&count),"HFP initial session count");
        for(int index=0;index<count;++index) {
            ComPtr<IAudioSessionControl> session;audio_check(sessions->GetSession(index,&session),"HFP initial session");
            events->OnSessionCreated(session.Get());
        }
    }
    ~SessionWatch() { manager->UnregisterSessionNotification(events.Get()); }
};
}
struct HfpActivity::Sessions {
    std::map<std::wstring,std::unique_ptr<SessionWatch>> watches;
};
HfpActivity::HfpActivity():sessions_(std::make_unique<Sessions>()) {
    DEVINST parent=0;
    auto result=CM_Locate_DevNodeW(&parent,const_cast<PWSTR>(target_device().instance.c_str()),CM_LOCATE_DEVNODE_NORMAL);
    if(result!=CR_SUCCESS) throw std::runtime_error("HFP Bluetooth parent lookup failed, CR="+std::to_string(result));
    DEVPROPTYPE type=0;ULONG size=sizeof(container_);
    result=CM_Get_DevNode_PropertyW(parent,&DEVPKEY_Device_ContainerId,&type,reinterpret_cast<PBYTE>(&container_),&size,0);
    if(result!=CR_SUCCESS || type!=DEVPROP_TYPE_GUID || size!=sizeof(container_))
        throw std::runtime_error("HFP Bluetooth container lookup failed, CR="+std::to_string(result));
}
HfpActivity::~HfpActivity()=default;
bool HfpActivity::active() const {
    ComPtr<IMMDeviceEnumerator> enumerator;
    audio_check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"HFP endpoint enumerator");
    ComPtr<IMMDeviceCollection> devices;
    audio_check(enumerator->EnumAudioEndpoints(eAll,DEVICE_STATE_ACTIVE,&devices),"HFP endpoints");
    UINT count=0;audio_check(devices->GetCount(&count),"HFP endpoint count");
    std::vector<std::wstring> current;
    bool found=false;
    for(UINT index=0;index<count;++index) {
        ComPtr<IMMDevice> device;audio_check(devices->Item(index,&device),"HFP endpoint");
        if(!matches(device.Get())) continue;
        auto adapter=audio_adapter_id(device.Get());
        std::transform(adapter.begin(),adapter.end(),adapter.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});
        if(adapter.find(L"bthhfenum#bthhfpaudio#")==std::wstring::npos) continue;
        LPWSTR raw_id=nullptr;audio_check(device->GetId(&raw_id),"HFP endpoint ID");
        const std::wstring id(raw_id);CoTaskMemFree(raw_id);
        current.push_back(id);
        auto& watch=sessions_->watches[id];
        if(!watch) watch=std::make_unique<SessionWatch>(device.Get());
        if(watch->events->active()) found=true;
    }
    std::erase_if(sessions_->watches,[&](const auto& entry){return std::find(current.begin(),current.end(),entry.first)==current.end();});
    return found;
}
bool HfpActivity::matches(IMMDevice* device) const {
    ComPtr<IPropertyStore> store;audio_check(device->OpenPropertyStore(STGM_READ,&store),"Headphone endpoint properties");
    PROPVARIANT value{};audio_check(store->GetValue(PKEY_Device_ContainerId,&value),"Headphone endpoint container");
    const bool result=value.vt==VT_CLSID && value.puuid && IsEqualGUID(*value.puuid,container_);
    PropVariantClear(&value);
    return result;
}
ComPtr<IMMDevice> HfpActivity::capture_device() const {
    ComPtr<IMMDeviceEnumerator> enumerator;
    audio_check(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,IID_PPV_ARGS(&enumerator)),"Microphone enumerator");
    ComPtr<IMMDeviceCollection> devices;
    audio_check(enumerator->EnumAudioEndpoints(eCapture,DEVICE_STATE_ACTIVE,&devices),"Microphone endpoints");
    UINT count=0;audio_check(devices->GetCount(&count),"Microphone endpoint count");
    ComPtr<IMMDevice> found;
    for(UINT index=0;index<count;++index) {
        ComPtr<IMMDevice> device;audio_check(devices->Item(index,&device),"Microphone endpoint");
        if(!matches(device.Get())) continue;
        auto adapter=audio_adapter_id(device.Get());
        std::transform(adapter.begin(),adapter.end(),adapter.begin(),[](wchar_t c){return static_cast<wchar_t>(towlower(c));});
        if(adapter.find(L"bthhfenum#bthhfpaudio#")==std::wstring::npos) continue;
        if(found) throw std::runtime_error("Multiple microphones for the target headphone");
        found=device;
    }
    return found;
}
void inspect_hfp_activity() {
    ComApartment apartment(COINIT_MULTITHREADED);
    const bool active=HfpActivity{}.active();
    std::cout<<"{\"event\":\"hfp_activity\",\"active\":"<<(active?"true":"false")<<"}\n";
}
}
