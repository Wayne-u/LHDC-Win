#include <initguid.h>
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <filesystem>
#include <iostream>
#include <vector>
#include <stdexcept>
#include "host/json.hpp"
#include "signature.hpp"
#include "audio/direct_pcm.hpp"
namespace {
constexpr GUID media_class={0x4d36e96c,0xe325,0x11ce,{0xbf,0xc1,0x08,0x00,0x2b,0xe1,0x03,0x18}};
[[noreturn]] void win_error(const char* operation) {
    throw std::runtime_error(std::string(operation)+" failed, Win32="+std::to_string(GetLastError()));
}
void require_admin() {
    SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;
    PSID sid=nullptr; BOOL member=FALSE;
    if (!AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&sid)) win_error("Administrator SID");
    const auto ok=CheckTokenMembership(nullptr,sid,&member); FreeSid(sid);
    if (!ok) win_error("Administrator query");
    if (!member) throw std::runtime_error("Audio device mutation requires an administrator process");
}
std::string utf8(const wchar_t* text) {
    const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,-1,nullptr,0,nullptr,nullptr);
    if (!size) win_error("Device text conversion size");
    std::string out(static_cast<std::size_t>(size),'\0');
    if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text,-1,out.data(),size,nullptr,nullptr)) win_error("Device text conversion");
    out.pop_back(); return out;
}
class Device {
public:
    explicit Device(const wchar_t* instance_id):instance_(instance_id) {
        set_=SetupDiCreateDeviceInfoList(&media_class,nullptr);
        if (set_==INVALID_HANDLE_VALUE) win_error("Audio device set");
        data_.cbSize=sizeof(data_);
    }
    ~Device() { SetupDiDestroyDeviceInfoList(set_); }
    Device(const Device&)=delete;
    bool open() {
        if (SetupDiOpenDeviceInfoW(set_,instance_.c_str(),nullptr,0,&data_)) return true;
        if (GetLastError()==ERROR_NO_SUCH_DEVINST) return false;
        win_error("Audio device lookup");
    }
    std::wstring property(const DEVPROPKEY& key) const {
        DEVPROPTYPE type=0; DWORD bytes=0;
        if (!SetupDiGetDevicePropertyW(set_,&data_,&key,&type,nullptr,0,&bytes,0)) {
            if (GetLastError()==ERROR_NOT_FOUND) return {};
            if (GetLastError()!=ERROR_INSUFFICIENT_BUFFER) win_error("Audio property size");
        }
        if (type!=DEVPROP_TYPE_STRING) throw std::runtime_error("Unexpected audio property type");
        std::vector<wchar_t> value(bytes/sizeof(wchar_t)+1);
        if (!SetupDiGetDevicePropertyW(set_,&data_,&key,&type,reinterpret_cast<PBYTE>(value.data()),bytes,nullptr,0)) win_error("Audio property");
        return value.data();
    }
    ULONG problem() const {
        ULONG flags=0,problem=0;
        const auto status=CM_Get_DevNode_Status(&flags,&problem,data_.DevInst,0);
        if (status!=CR_SUCCESS) throw std::runtime_error("Audio devnode status failed, CONFIGRET="+std::to_string(status));
        return problem;
    }
    void snapshot() const {
        std::cout << "{\"event\":\"audio_device\",\"present\":true,\"instance\":" << lhdc::json_string(utf8(instance_.c_str()))
            << ",\"service\":" << lhdc::json_string(utf8(property(DEVPKEY_Device_Service).c_str()))
            << ",\"inf\":" << lhdc::json_string(utf8(property(DEVPKEY_Device_DriverInfPath).c_str())) << ",\"problem\":" << problem() << "}\n";
    }
    bool install(const std::filesystem::path& inf,const wchar_t* expected_service) {
        SP_DEVINSTALL_PARAMS_W params{sizeof(params)};
        if (!SetupDiGetDeviceInstallParamsW(set_,&data_,&params)) win_error("Audio install parameters");
        const auto path=inf.wstring();
        if (path.size()>=std::size(params.DriverPath)) throw std::invalid_argument("Audio INF path exceeds SetupAPI limit");
        params.Flags|=DI_ENUMSINGLEINF;
        std::copy(path.begin(),path.end(),params.DriverPath); params.DriverPath[path.size()]=L'\0';
        if (!SetupDiSetDeviceInstallParamsW(set_,&data_,&params)) win_error("Audio INF selection");
        if (!SetupDiBuildDriverInfoList(set_,&data_,SPDIT_COMPATDRIVER)) win_error("Audio compatible driver list");
        SP_DRVINFO_DATA_W driver{sizeof(driver)},extra{sizeof(extra)};
        if (!SetupDiEnumDriverInfoW(set_,&data_,SPDIT_COMPATDRIVER,0,&driver)) win_error("Audio driver selection");
        if (SetupDiEnumDriverInfoW(set_,&data_,SPDIT_COMPATDRIVER,1,&extra) || GetLastError()!=ERROR_NO_MORE_ITEMS) throw std::runtime_error("Expected one compatible audio child driver");
        BOOL reboot=FALSE;
        if (!DiInstallDevice(nullptr,set_,&data_,&driver,DIIDFLAG_NOFINISHINSTALLUI,&reboot)) win_error("Audio single-instance install");
        snapshot();
        if (property(DEVPKEY_Device_Service)!=expected_service || problem()!=0)
            throw std::runtime_error("Audio child driver binding did not verify");
        return reboot!=FALSE;
    }
private:
    HDEVINFO set_;
    std::wstring instance_;
    mutable SP_DEVINFO_DATA data_{};
};
}
int wmain(int argc,wchar_t** argv) {
    try {
        if (argc<2) throw std::invalid_argument("Usage: lhdc-audio-device verify-package DIRECTORY CERTIFICATE | stage INF CERTIFICATE | install-child INSTANCE INF CERTIFICATE");
        const std::wstring command=argv[1];
        if (command==L"verify-package" && argc==4) { lhdc::verify_package_signature(argv[2],argv[3],L"lhdc-audio"); return 0; }
        if (command==L"stage" && argc==4) {
            require_admin();
            const auto inf=std::filesystem::absolute(argv[2]);
            lhdc::verify_package_signature(inf.parent_path(),argv[3],L"lhdc-audio");
            wchar_t destination[MAX_PATH]{};
            if (!SetupCopyOEMInfW(inf.c_str(),nullptr,SPOST_PATH,0,destination,MAX_PATH,nullptr,nullptr)) win_error("Audio child package staging");
            std::cout << "{\"event\":\"audio_package_staged\",\"inf\":" << lhdc::json_string(utf8(destination)) << "}\n";
            return 0;
        }
        if (command==L"install-child" && argc==5) {
            require_admin();
            if (_wcsnicmp(argv[2],L"LHDCWIN\\AUDIO\\",14)!=0) throw std::runtime_error("Expected an LHDC audio child instance");
            const auto inf=std::filesystem::absolute(argv[3]);
            lhdc::verify_package_signature(inf.parent_path(),argv[4],L"lhdc-audio");
            Device child(argv[2]);
            if (!child.open() || _wcsicmp(child.property(DEVPKEY_Device_Parent).c_str(),lhdc::target_device().instance.c_str())!=0)
                throw std::runtime_error("Audio child does not belong to the selected headphones");
            const auto reboot=child.install(inf,L"lhdc-render");
            std::cout << "{\"event\":\"audio_child_binding\",\"binding_verified\":true,\"reboot_required\":" << (reboot?"true":"false") << "}\n";
            return reboot?3:0;
        }
        throw std::invalid_argument("Invalid audio device command");
    } catch (const std::exception& error) {
        std::cerr << "{\"event\":\"error\",\"message\":" << lhdc::json_string(error.what()) << "}\n"; return 1;
    }
}
