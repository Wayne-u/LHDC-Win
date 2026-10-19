#include <initguid.h>
#include "windows.hpp"
#include "radio.hpp"
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <bluetoothapis.h>
#include <objbase.h>
#include <stdexcept>
#include <iostream>
#include <memory>
#include <cstring>
#include <chrono>
#include <cwctype>
#include <iomanip>
#include <algorithm>
#include "host/json.hpp"
namespace lhdc {
using detail::query_connection;
static void win_error(const char* operation) {
    throw WindowsError(operation,GetLastError());
}
static std::size_t channel_index(ULONG channel) {
    if (channel<LHDC_CHANNEL_SIGNAL || channel>LHDC_CHANNEL_COUNT) throw std::invalid_argument("Invalid transport channel ID");
    return channel-1;
}
static std::string utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if (!size) win_error("UTF-8 conversion");
    std::string result(size,'\0');
    if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),size,nullptr,nullptr)) win_error("UTF-8 conversion");
    return result;
}
struct DeviceSet {
    HDEVINFO value;
    ~DeviceSet() { if (value!=INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(value); }
};
static std::wstring property(HDEVINFO set,SP_DEVINFO_DATA& device,const DEVPROPKEY& key) {
    DEVPROPTYPE type=0;
    DWORD size=0;
    if (!SetupDiGetDevicePropertyW(set,&device,&key,&type,nullptr,0,&size,0)) {
        if (GetLastError()==ERROR_NOT_FOUND) return {};
        if (GetLastError()!=ERROR_INSUFFICIENT_BUFFER) win_error("Device property size");
    }
    if (type!=DEVPROP_TYPE_STRING && type!=DEVPROP_TYPE_STRING_LIST) throw std::runtime_error("Unexpected PnP property type");
    std::vector<wchar_t> buffer(size/sizeof(wchar_t)+1,L'\0');
    if (!SetupDiGetDevicePropertyW(set,&device,&key,&type,reinterpret_cast<PBYTE>(buffer.data()),size,&size,0)) win_error("Device property");
    return std::wstring(buffer.data());
}
struct DeviceInterface { std::wstring path; DEVINST device; };
static std::vector<DeviceInterface> interfaces(const GUID& type=GUID_DEVINTERFACE_LHDC_TRANSPORT) {
    DeviceSet set{SetupDiGetClassDevsW(&type,nullptr,nullptr,DIGCF_PRESENT|DIGCF_DEVICEINTERFACE)};
    if (set.value==INVALID_HANDLE_VALUE) win_error("Device interface enumeration");
    std::vector<DeviceInterface> paths;
    for (DWORD i=0;;++i) {
        SP_DEVICE_INTERFACE_DATA data{sizeof(data)};
        if (!SetupDiEnumDeviceInterfaces(set.value,nullptr,&type,i,&data)) {
            if (GetLastError()==ERROR_NO_MORE_ITEMS) break;
            win_error("Device interface enumeration");
        }
        DWORD size=0;
        SetupDiGetDeviceInterfaceDetailW(set.value,&data,nullptr,0,&size,nullptr);
        if (GetLastError()!=ERROR_INSUFFICIENT_BUFFER) win_error("Device interface size");
        std::vector<std::uint8_t> buffer(size);
        auto detail=reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize=sizeof(*detail);
        SP_DEVINFO_DATA device{sizeof(device)};
        if (!SetupDiGetDeviceInterfaceDetailW(set.value,&data,detail,size,nullptr,&device)) win_error("Device interface path");
        paths.push_back({detail->DevicePath,device.DevInst});
    }
    return paths;
}
void inspect_windows() {
    std::ostringstream out;
    const auto radios=interfaces(GUID_BTHPORT_DEVICE_INTERFACE);
    DeviceSet set{SetupDiGetClassDevsW(nullptr,nullptr,nullptr,DIGCF_ALLCLASSES)};
    if (set.value==INVALID_HANDLE_VALUE) win_error("PnP enumeration");
    out << "{\"devices\":[";
    bool first=true;
    for (DWORD i=0;;++i) {
        SP_DEVINFO_DATA device{sizeof(device)};
        if (!SetupDiEnumDeviceInfo(set.value,i,&device)) {
            if (GetLastError()==ERROR_NO_MORE_ITEMS) break;
            win_error("Device enumeration");
        }
        std::vector<wchar_t> id(MAX_DEVICE_ID_LEN);
        if (!SetupDiGetDeviceInstanceIdW(set.value,&device,id.data(),static_cast<DWORD>(id.size()),nullptr)) win_error("Device instance ID");
        std::wstring instance(id.data());
        std::wstring upper(instance);
        CharUpperBuffW(upper.data(),static_cast<DWORD>(upper.size()));
        const bool radio=std::any_of(radios.begin(),radios.end(),[&](const auto& item){return item.device==device.DevInst;});
        if (upper.find(L"BTHENUM\\{0000110B-")!=0 && !radio) continue;
        if (!first) out << ',';
        first=false;
        ULONG status=0,problem=0;
        const auto cr=CM_Get_DevNode_Status(&status,&problem,device.DevInst,0);
        out << "{\"instance_id\":" << json_string(utf8(instance))
            << ",\"name\":" << json_string(utf8(property(set.value,device,DEVPKEY_Device_FriendlyName)))
            << ",\"description\":" << json_string(utf8(property(set.value,device,DEVPKEY_Device_DeviceDesc)))
            << ",\"service\":" << json_string(utf8(property(set.value,device,DEVPKEY_Device_Service)))
            << ",\"inf\":" << json_string(utf8(property(set.value,device,DEVPKEY_Device_DriverInfPath)))
            << ",\"hardware_id\":" << json_string(utf8(property(set.value,device,DEVPKEY_Device_HardwareIds)))
            << ",\"driver_version\":" << json_string(utf8(property(set.value,device,DEVPKEY_Device_DriverVersion)))
            << ",\"cm_result\":" << cr << ",\"devnode_status\":" << status << ",\"problem\":" << problem << '}';
    }
    const auto paths=interfaces();
    out << "],\"transport_interfaces\":[";
    for (std::size_t i=0;i<paths.size();++i) {
        if (i) out << ',';
        out << json_string(utf8(paths[i].path));
    }
    out << "],\"peer_capabilities_observed\":false}\n";
    std::cout << out.str();
}
static std::wstring target_override;
bool absolute_volume_enabled() {
    DWORD enabled=0,size=sizeof(enabled);
    const auto status=RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\LHDC-Win\\Device",L"AbsoluteVolume",RRF_RT_REG_DWORD,nullptr,&enabled,&size);
    if (status==ERROR_FILE_NOT_FOUND) return false;
    if (status!=ERROR_SUCCESS) throw WindowsError("Volume mode read",status);
    if (enabled>1) throw std::runtime_error("Invalid volume mode");
    return enabled!=0;
}
void claim_avrcp_controller() {
    if (!absolute_volume_enabled()) return;
    wchar_t instance[MAX_DEVICE_ID_LEN]{};DWORD size=sizeof(instance);
    const auto error=RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\LHDC-Win\\Device",L"NativeAvrcpInstance",RRF_RT_REG_SZ,nullptr,instance,&size);
    if (error==ERROR_FILE_NOT_FOUND) return;
    if (error!=ERROR_SUCCESS) throw WindowsError("Native AVRCP identity read",error);
    DEVINST device=0;auto status=CM_Locate_DevNodeW(&device,instance,CM_LOCATE_DEVNODE_NORMAL);
    if (status==CR_NO_SUCH_DEVNODE) return;
    if (status!=CR_SUCCESS) throw std::runtime_error("Native AVRCP lookup failed, CR="+std::to_string(status));
    ULONG flags=0,problem=0;status=CM_Get_DevNode_Status(&flags,&problem,device,0);
    if (status!=CR_SUCCESS) throw std::runtime_error("Native AVRCP status failed, CR="+std::to_string(status));
    if (problem==CM_PROB_DISABLED) return;
    if (problem) throw std::runtime_error("Native AVRCP controller has a device problem="+std::to_string(problem));
    status=CM_Disable_DevNode(device,CM_DISABLE_UI_NOT_OK|CM_DISABLE_PERSIST);
    if (status!=CR_SUCCESS) throw std::runtime_error("Native AVRCP controller claim failed, CR="+std::to_string(status));
}
void override_target(std::wstring instance) { target_override=std::move(instance); }
static std::wstring selected_instance() {
    if (!target_override.empty()) return target_override;
    DWORD size=0;
    auto status=RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\LHDC-Win\\Device",L"InstanceId",RRF_RT_REG_SZ,nullptr,nullptr,&size);
    if (status==ERROR_FILE_NOT_FOUND) return {};
    if (status!=ERROR_SUCCESS) throw WindowsError("Selected device size",status);
    std::wstring instance(size/sizeof(wchar_t),L'\0');
    status=RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\LHDC-Win\\Device",L"InstanceId",RRF_RT_REG_SZ,nullptr,instance.data(),&size);
    if (status!=ERROR_SUCCESS) throw WindowsError("Selected device read",status);
    instance.resize(wcslen(instance.c_str()));
    return instance;
}
std::vector<TargetDevice> discover_targets() {
    DeviceSet set{SetupDiGetClassDevsW(nullptr,L"BTHENUM",nullptr,DIGCF_ALLCLASSES|DIGCF_PRESENT)};
    if(set.value==INVALID_HANDLE_VALUE) win_error("Target device enumeration");
    std::vector<TargetDevice> found;
    for(DWORD index=0;;++index) {
        SP_DEVINFO_DATA device{sizeof(device)};
        if(!SetupDiEnumDeviceInfo(set.value,index,&device)) {
            if(GetLastError()==ERROR_NO_MORE_ITEMS) break;
            win_error("Target device enumeration");
        }
        wchar_t id[MAX_DEVICE_ID_LEN]{};
        if(!SetupDiGetDeviceInstanceIdW(set.value,&device,id,MAX_DEVICE_ID_LEN,nullptr)) win_error("Target instance ID");
        std::wstring instance(id);
        const auto address=audio_sink_address(instance);
        if (!address) continue;
        const auto connection=query_connection(*address);
        if (!connection.known || (!connection.device.fRemembered && !connection.device.fAuthenticated)) continue;
        GUID container{};DEVPROPTYPE type=0;DWORD size=sizeof(container);
        if (!SetupDiGetDevicePropertyW(set.value,&device,&DEVPKEY_Device_ContainerId,&type,reinterpret_cast<PBYTE>(&container),size,&size,0)) win_error("Target container");
        if (type!=DEVPROP_TYPE_GUID || size!=sizeof(container)) throw std::runtime_error("Invalid Bluetooth container ID");
        wchar_t container_text[40]{};
        if (!StringFromGUID2(container,container_text,40)) throw std::runtime_error("Bluetooth container conversion failed");
        const auto hardware=property(set.value,device,DEVPKEY_Device_HardwareIds);
        const bool melody=same_instance(hardware,L"BTHENUM\\{0000110B-0000-1000-8000-00805F9B34FB}_VID&000102B0_PID&0000") &&
            same_instance(connection.device.szName,L"OPPO Enco X4");
        found.push_back({std::move(instance),*address,connection.device.szName,container_text,
            property(set.value,device,DEVPKEY_Device_Service),connection.device.fConnected!=FALSE,melody});
    }
    return found;
}
std::optional<TargetDevice> discover_target() {
    const auto devices=discover_targets();
    const auto index=target_index(devices,selected_instance());
    return index?std::optional{devices[*index]}:std::nullopt;
}
const TargetDevice& target_device() {
    static const auto target=[] {
        const auto device=discover_target();
        if(!device) throw std::runtime_error("Select a paired Bluetooth Audio Sink in the panel or use --device INSTANCE");
        return *device;
    }();
    return target;
}
static void print_target(const TargetDevice& target) {
    std::ostringstream address;
    address<<std::hex<<std::uppercase<<std::setfill('0');
    for (int byte=5;byte>=0;--byte) {
        if (byte!=5) address<<':';
        address<<std::setw(2)<<((target.address>>(byte*8))&255);
    }
    std::cout<<"\"instance\":"<<json_string(utf8(target.instance))<<",\"address\":"<<json_string(address.str())
        <<",\"name\":"<<json_string(utf8(target.name))<<",\"container_id\":"<<json_string(utf8(target.container))
        <<",\"service\":"<<json_string(utf8(target.service))<<",\"connected\":"<<(target.connected?"true":"false")
        <<",\"hey_melody\":"<<(target.hey_melody?"true":"false");
}
void inspect_targets() {
    const auto devices=discover_targets();
    const auto index=target_index(devices,selected_instance());
    std::cout<<"{\"event\":\"target_devices\",\"selected_instance\":"<<json_string(index?utf8(devices[*index].instance):"")<<",\"devices\":[";
    for (std::size_t i=0;i<devices.size();++i) {
        if (i) std::cout<<',';
        std::cout<<'{';print_target(devices[i]);std::cout<<'}';
    }
    std::cout<<"]}\n";
}
void inspect_target() {
    const auto target=discover_target();
    std::cout<<"{\"event\":\"target_device\",\"found\":"<<(target?"true":"false");
    if(target) { std::cout<<',';print_target(*target); }
    std::cout<<"}\n";
}
bool bluetooth_connected(std::uint64_t address) { return query_connection(address).device.fConnected!=FALSE; }
void inspect_connection(std::uint64_t address) {
    const auto connection=query_connection(address); const auto& info=connection.device;
    std::cout << "{\"event\":\"bluetooth_connection\",\"radio_present\":" << (connection.radio?"true":"false") << ",\"known\":" << (connection.known?"true":"false")
        << ",\"connected\":" << (info.fConnected?"true":"false") << ",\"remembered\":" << (info.fRemembered?"true":"false")
        << ",\"authenticated\":" << (info.fAuthenticated?"true":"false") << ",\"name\":" << json_string(utf8(info.szName))
        << ",\"audio_stream_verified\":false}\n";
}
Transport::Transport(std::uint64_t address):address_(address) {
    auto paths=interfaces();
    const auto& selected=target_device().instance;
    std::erase_if(paths,[&](const auto& path) {
        wchar_t instance[MAX_DEVICE_ID_LEN]{};
        const auto status=CM_Get_Device_IDW(path.device,instance,MAX_DEVICE_ID_LEN,0);
        if (status!=CR_SUCCESS) throw std::runtime_error("Transport instance lookup failed, CR="+std::to_string(status));
        return !same_instance(instance,selected);
    });
    if (paths.empty()) throw std::runtime_error("LHDC transport driver is not bound/loaded for the selected Audio Sink");
    if (paths.size()!=1) throw std::runtime_error("Multiple LHDC transport interfaces belong to the selected Audio Sink");
    handle_=CreateFileW(paths[0].path.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_OVERLAPPED,nullptr);
    if (handle_==INVALID_HANDLE_VALUE) win_error("Transport open");
    try {
        if (info().RemoteAddress!=address_) throw std::runtime_error("Transport remote address does not match the selected earphone");
    } catch (...) { CloseHandle(handle_); handle_=INVALID_HANDLE_VALUE; throw; }
}
Transport::~Transport() { if (handle_!=INVALID_HANDLE_VALUE) CloseHandle(handle_); }
void Transport::audio_present(bool present) {
    LHDC_AUDIO_PRESENT_INPUT input{{sizeof(input),LHDC_ABI_VERSION},static_cast<ULONG>(present),0};
    ioctl(IOCTL_LHDC_AUDIO_PRESENT,&input,sizeof(input),nullptr,0);
}
DWORD Transport::ioctl(DWORD code,void* input,DWORD input_size,void* output,DWORD output_size,DWORD timeout_ms) {
    OVERLAPPED overlapped{};
    overlapped.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!overlapped.hEvent) win_error("I/O event");
    struct Event { HANDLE value; ~Event(){CloseHandle(value);} } event{overlapped.hEvent};
    DWORD transferred=0;
    const auto completed=DeviceIoControl(handle_,code,input,input_size,output,output_size,&transferred,&overlapped);
    const auto ioctl_error=completed ? ERROR_SUCCESS : GetLastError();
    if (!completed) {
        if (ioctl_error!=ERROR_IO_PENDING) throw WindowsError("Transport IOCTL",ioctl_error);
        const auto wait=WaitForSingleObject(overlapped.hEvent,timeout_ms);
        const auto wait_error=wait==WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
        if (wait!=WAIT_OBJECT_0) {
            CancelIoEx(handle_,&overlapped);
            // OVERLAPPED, event, and user buffers must survive cancellation completion.
            if (GetOverlappedResult(handle_,&overlapped,&transferred,TRUE) && wait==WAIT_TIMEOUT) return transferred;
            throw WindowsError("Transport I/O wait",wait_error);
        }
        if (!GetOverlappedResult(handle_,&overlapped,&transferred,FALSE)) {
            const auto error=GetLastError();
            std::ostringstream operation;
            operation<<"Transport completion (IOCTL=0x"<<std::hex<<code<<", NTSTATUS=0x"<<overlapped.Internal<<")";
            throw WindowsError(operation.str().c_str(),error);
        }
    }
    return transferred;
}
LHDC_INFO Transport::info(ULONG channel) {
    channel_index(channel);
    LHDC_CHANNEL_INPUT input{{sizeof(input),LHDC_ABI_VERSION},channel,0};
    LHDC_INFO result{};
    const auto bytes=ioctl(IOCTL_LHDC_QUERY_INFO,&input,sizeof(input),&result,sizeof(result));
    if (bytes!=sizeof(result) || result.Header.Version!=LHDC_ABI_VERSION || result.Header.Size!=sizeof(result) || result.ChannelId!=channel)
        throw std::runtime_error("Transport info ABI mismatch");
    return result;
}
void Transport::open(ULONG channel) {
    const auto index=channel_index(channel);
    LHDC_OPEN_INPUT input{{sizeof(input),LHDC_ABI_VERSION},address_,channel,0};
    ioctl(IOCTL_LHDC_OPEN,&input,sizeof(input),nullptr,0);
    const auto state=info(channel);
    if (!state.Connected || state.ChannelId!=channel || !state.InMtu || !state.OutMtu || state.InMtu>LHDC_MAX_SDU || state.OutMtu>LHDC_MAX_SDU)
        throw std::runtime_error("Transport opened without valid MTUs");
    in_mtu_[index]=state.InMtu; out_mtu_[index]=state.OutMtu; connected_[index]=true;
}
void Transport::close(ULONG channel) {
    const auto index=channel_index(channel);
    if (!connected_[index]) return;
    LHDC_CHANNEL_INPUT input{{sizeof(input),LHDC_ABI_VERSION},channel,0};
    ioctl(IOCTL_LHDC_CLOSE,&input,sizeof(input),nullptr,0);
    connected_[index]=false;
}
Transport::PendingSend::PendingSend(HANDLE handle,std::vector<std::uint8_t> input):handle_(handle),input_(std::move(input)) {
    overlapped_.hEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    if (!overlapped_.hEvent) win_error("Send event");
}
Transport::PendingSend::~PendingSend() {
    if (active_) {
        CancelIoEx(handle_,&overlapped_);
        DWORD transferred=0;
        // Even cancellation must finish before releasing OVERLAPPED and buffers.
        GetOverlappedResult(handle_,&overlapped_,&transferred,TRUE);
    }
    CloseHandle(overlapped_.hEvent);
}
std::int64_t Transport::PendingSend::pending_age_us() const {
    if (!active_) return 0;
    const auto result=WaitForSingleObject(overlapped_.hEvent,0);
    if (result==WAIT_OBJECT_0) return 0;
    if (result!=WAIT_TIMEOUT) win_error("Send completion status");
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-submitted_).count();
}
SendTiming Transport::PendingSend::wait(DWORD timeout_ms) {
    if (!active_) return timing_;
    // Completed requests may remain in the window until a later packet retires
    // them. Their retention time is not Bluetooth congestion.
    timing_.pending_age_us=pending_age_us();
    const auto started=std::chrono::steady_clock::now();
    const auto age_ms=static_cast<std::uint64_t>(timing_.pending_age_us)/1000;
    const auto remaining=age_ms>=timeout_ms ? 0 : timeout_ms-static_cast<DWORD>(age_ms);
    const auto result=WaitForSingleObject(overlapped_.hEvent,remaining);
    const auto error=result==WAIT_FAILED ? GetLastError() : ERROR_TIMEOUT;
    const auto waited=std::chrono::steady_clock::now();
    timing_.wait_us=std::chrono::duration_cast<std::chrono::microseconds>(waited-started).count();
    if (timing_.pending_age_us) timing_.pending_age_us+=timing_.wait_us;
    DWORD transferred=0;
    if (result!=WAIT_OBJECT_0) {
        CancelIoEx(handle_,&overlapped_);
        GetOverlappedResult(handle_,&overlapped_,&transferred,TRUE);
        active_=false;
        throw WindowsError("Media send wait",error);
    }
    const auto completed=GetOverlappedResult(handle_,&overlapped_,&transferred,FALSE);
    const auto completion_error=completed ? ERROR_SUCCESS : GetLastError();
    active_=false;
    timing_.completion_us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-waited).count();
    if (!completed) throw WindowsError("Send completion",completion_error);
    return timing_;
}
std::unique_ptr<Transport::PendingSend> Transport::begin_send(std::span<const std::uint8_t> bytes,ULONG channel) {
    const auto index=channel_index(channel);
    if (!connected_[index] || bytes.empty() || bytes.size()>out_mtu_[index]) throw std::invalid_argument("SDU exceeds channel MTU or channel is closed");
    std::vector<std::uint8_t> input(sizeof(LHDC_SDU_HEADER)+bytes.size());
    LHDC_SDU_HEADER header{{static_cast<ULONG>(input.size()),LHDC_ABI_VERSION},channel,static_cast<ULONG>(bytes.size())};
    std::memcpy(input.data(),&header,sizeof(header));
    std::memcpy(input.data()+sizeof(header),bytes.data(),bytes.size());
    auto request=std::unique_ptr<PendingSend>(new PendingSend(handle_,std::move(input)));
    DWORD transferred=0;
    const auto started=std::chrono::steady_clock::now();
    request->submitted_=started;
    const auto completed=DeviceIoControl(handle_,IOCTL_LHDC_SEND,request->input_.data(),static_cast<DWORD>(request->input_.size()),nullptr,0,&transferred,&request->overlapped_);
    const auto error=completed ? ERROR_SUCCESS : GetLastError();
    request->timing_.submit_us=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-started).count();
    if (!completed && error!=ERROR_IO_PENDING) throw WindowsError("Send submission",error);
    request->active_=!completed;
    request->timing_.pending=!completed;
    return request;
}
SendTiming Transport::send(std::span<const std::uint8_t> bytes,ULONG channel) {
    return begin_send(bytes,channel)->wait();
}
std::vector<std::uint8_t> Transport::receive(ULONG channel,DWORD timeout_ms) {
    const auto index=channel_index(channel);
    if (!connected_[index]) throw std::runtime_error("Channel is closed");
    LHDC_SDU_HEADER input{{sizeof(input),LHDC_ABI_VERSION},channel,0};
    std::vector<std::uint8_t> output(sizeof(input)+in_mtu_[index]);
    const auto bytes=ioctl(IOCTL_LHDC_RECEIVE,&input,sizeof(input),output.data(),static_cast<DWORD>(output.size()),timeout_ms);
    if (bytes<sizeof(input)) throw std::runtime_error("Truncated transport SDU header");
    std::memcpy(&input,output.data(),sizeof(input));
    if (input.Header.Version!=LHDC_ABI_VERSION || input.Header.Size!=bytes || input.ChannelId!=channel || input.PayloadSize!=bytes-sizeof(input))
        throw std::runtime_error("Transport SDU ABI mismatch");
    return std::vector<std::uint8_t>(output.begin()+sizeof(input),output.begin()+bytes);
}
void Transport::cancel_pending() {
    if (!CancelIoEx(handle_,nullptr) && GetLastError()!=ERROR_NOT_FOUND) win_error("Transport cancellation");
}
}
