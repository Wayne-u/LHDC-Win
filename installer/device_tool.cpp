#include "transport/windows.hpp"
#include <initguid.h>
#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <winternl.h>
#include <filesystem>
#include <iostream>
#include <vector>
#include <stdexcept>
#include <string>
#include <algorithm>
#include "host/json.hpp"
#include "signature.hpp"

namespace {
constexpr wchar_t standard_inf[]=L"microsoft_bluetooth_a2dp_src.inf";

[[noreturn]] void win_error(const char* operation) {
    throw std::runtime_error(std::string(operation)+" failed, Win32="+std::to_string(GetLastError()));
}
std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const auto size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),nullptr,0,nullptr,nullptr);
    if (!size) win_error("UTF-8 size");
    std::string result(size,'\0');
    if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),static_cast<int>(text.size()),result.data(),size,nullptr,nullptr)) win_error("UTF-8 conversion");
    return result;
}
bool equal(const std::wstring& left,const std::wstring& right) {
    return CompareStringOrdinal(left.c_str(),-1,right.c_str(),-1,TRUE)==CSTR_EQUAL;
}
bool administrator() {
    SID_IDENTIFIER_AUTHORITY authority=SECURITY_NT_AUTHORITY;
    PSID sid=nullptr;
    BOOL member=FALSE;
    if (!AllocateAndInitializeSid(&authority,2,SECURITY_BUILTIN_DOMAIN_RID,DOMAIN_ALIAS_RID_ADMINS,0,0,0,0,0,0,&sid)) win_error("Administrator SID");
    const auto ok=CheckTokenMembership(nullptr,sid,&member);
    FreeSid(sid);
    if (!ok) win_error("Administrator token");
    return member!=FALSE;
}
ULONG code_integrity_options() {
    SYSTEM_CODEINTEGRITY_INFORMATION info{sizeof(info),0};
    ULONG length=0;
    const auto status=NtQuerySystemInformation(SystemCodeIntegrityInformation,&info,sizeof(info),&length);
    if (status<0) throw std::runtime_error("Code Integrity query failed, NTSTATUS="+std::to_string(static_cast<ULONG>(status)));
    if (length!=sizeof(info)) throw std::runtime_error("Unexpected Code Integrity structure size");
    return info.CodeIntegrityOptions;
}
void readiness() {
    const auto flags=code_integrity_options();
    std::cout << "{\"event\":\"readiness\",\"administrator\":" << (administrator()?"true":"false")
        << ",\"code_integrity_options\":" << flags
        << ",\"test_signing_active\":" << ((flags&CODEINTEGRITY_OPTION_TESTSIGN)?"true":"false")
        << ",\"hvci_kernel_enforced\":" << ((flags&CODEINTEGRITY_OPTION_HVCI_KMCI_ENABLED)?"true":"false") << "}\n";
}
class Target {
public:
    explicit Target(const wchar_t* instance) {
        // Resolve the supported physical device independently before any operation.
        if (!equal(instance,lhdc::target_device().instance)) throw std::invalid_argument("Only the automatically detected Enco X4 Audio Sink instance is allowed");
        set_=SetupDiCreateDeviceInfoList(nullptr,nullptr);
        if (set_==INVALID_HANDLE_VALUE) win_error("Device set");
        data_.cbSize=sizeof(data_);
        if (!SetupDiOpenDeviceInfoW(set_,instance,nullptr,0,&data_)) {
            const auto error=GetLastError();
            SetupDiDestroyDeviceInfoList(set_);
            set_=INVALID_HANDLE_VALUE;
            SetLastError(error);win_error("Target instance");
        }
    }
    ~Target() { if (set_!=INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(set_); }
    Target(const Target&)=delete;
    Target& operator=(const Target&)=delete;
    std::wstring property(const DEVPROPKEY& key) {
        DWORD size=0;
        DEVPROPTYPE type=0;
        if (!SetupDiGetDevicePropertyW(set_,&data_,&key,&type,nullptr,0,&size,0)) {
            if (GetLastError()!=ERROR_INSUFFICIENT_BUFFER) win_error("Target property size");
        }
        if (type!=DEVPROP_TYPE_STRING) throw std::runtime_error("Unexpected target property type");
        std::vector<wchar_t> value(size/sizeof(wchar_t)+1,L'\0');
        if (!SetupDiGetDevicePropertyW(set_,&data_,&key,&type,reinterpret_cast<PBYTE>(value.data()),size,nullptr,0)) win_error("Target property");
        return std::wstring(value.data());
    }
    ULONG problem() {
        ULONG flags=0,problem=0;
        const auto result=CM_Get_DevNode_Status(&flags,&problem,data_.DevInst,0);
        if (result!=CR_SUCCESS) throw std::runtime_error("Target devnode query failed, CONFIGRET="+std::to_string(result));
        return problem;
    }
    bool clear_transport_security() {
        DWORD size=0,type=0;
        if (!SetupDiGetDeviceRegistryPropertyW(set_,&data_,SPDRP_SECURITY_SDS,&type,nullptr,0,&size)) {
            if (GetLastError()==ERROR_INVALID_DATA) return false; // No per-device override.
            if (GetLastError()!=ERROR_INSUFFICIENT_BUFFER) win_error("Device security size");
        }
        std::vector<wchar_t> value(size/sizeof(wchar_t)+1,L'\0');
        if (!SetupDiGetDeviceRegistryPropertyW(set_,&data_,SPDRP_SECURITY_SDS,&type,reinterpret_cast<PBYTE>(value.data()),size,nullptr))
            win_error("Device security query");
        // The transport INF restricts its private IOCTLs to SYSTEM/admins.
        // That override must not survive on the inbox audio device: Audiosrv
        // runs as LocalService and needs the inbox driver's default access.
        if (!equal(value.data(),L"D:P(A;;GA;;;SY)(A;;GA;;;BA)")) return false;
        if (!SetupDiSetDeviceRegistryPropertyW(set_,&data_,SPDRP_SECURITY,nullptr,0)) win_error("Clear transport device security");
        std::cout << "{\"event\":\"transport_security_removed\"}\n";
        return true;
    }
    void reset_inbox_access() {
        // Match the default access observed on the inbox Bluetooth audio
        // filters. Explicitly apply it before reloading: deleting a registry
        // override alone leaves the existing PDO's access check unchanged.
        constexpr wchar_t access[]=L"D:(A;;0x1201bf;;;WD)(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;RC)";
        if (!SetupDiSetDeviceRegistryPropertyW(set_,&data_,SPDRP_SECURITY_SDS,reinterpret_cast<const BYTE*>(access),sizeof(access)))
            win_error("Restore inbox audio device access");
    }
    void snapshot(const char* event) {
        const auto service=property(DEVPKEY_Device_Service);
        const auto inf=property(DEVPKEY_Device_DriverInfPath);
        const auto code=problem();
        std::cout << "{\"event\":" << lhdc::json_string(event) << ",\"instance_id\":" << lhdc::json_string(utf8(lhdc::target_device().instance.c_str()))
            << ",\"service\":" << lhdc::json_string(utf8(service)) << ",\"inf\":" << lhdc::json_string(utf8(inf))
            << ",\"problem\":" << code << "}\n";
    }
    std::vector<SP_DRVINFO_DATA_W> build_candidates(const std::filesystem::path& inf) {
        if (!std::filesystem::is_regular_file(inf)) throw std::invalid_argument("INF file does not exist");
        const auto path=std::filesystem::absolute(inf).wstring();
        SP_DEVINSTALL_PARAMS_W params{sizeof(params)};
        if (!SetupDiGetDeviceInstallParamsW(set_,&data_,&params)) win_error("Device install parameters");
        if (path.size()>=std::size(params.DriverPath)) throw std::invalid_argument("INF path exceeds SetupAPI limit");
        params.Flags|=DI_ENUMSINGLEINF;
        std::copy(path.begin(),path.end(),params.DriverPath);
        params.DriverPath[path.size()]=L'\0';
        if (!SetupDiSetDeviceInstallParamsW(set_,&data_,&params)) win_error("Single INF selection");
        if (!SetupDiBuildDriverInfoList(set_,&data_,SPDIT_COMPATDRIVER)) win_error("Compatible driver list");
        std::vector<SP_DRVINFO_DATA_W> candidates;
        for (DWORD i=0;;++i) {
            SP_DRVINFO_DATA_W driver{sizeof(driver)};
            if (!SetupDiEnumDriverInfoW(set_,&data_,SPDIT_COMPATDRIVER,i,&driver)) {
                if (GetLastError()==ERROR_NO_MORE_ITEMS) break;
                win_error("Compatible driver enumeration");
            }
            candidates.push_back(driver);
        }
        return candidates;
    }
    void print_candidate(SP_DRVINFO_DATA_W& driver) {
        DWORD size=0;
        SetupDiGetDriverInfoDetailW(set_,&data_,&driver,nullptr,0,&size);
        if (GetLastError()!=ERROR_INSUFFICIENT_BUFFER) win_error("Driver detail size");
        std::vector<BYTE> buffer(size);
        auto detail=reinterpret_cast<SP_DRVINFO_DETAIL_DATA_W*>(buffer.data());
        detail->cbSize=sizeof(*detail);
        if (!SetupDiGetDriverInfoDetailW(set_,&data_,&driver,detail,size,nullptr)) win_error("Driver detail");
        std::cout << "{\"event\":\"candidate\",\"description\":" << lhdc::json_string(utf8(driver.Description))
            << ",\"provider\":" << lhdc::json_string(utf8(driver.ProviderName))
            << ",\"inf_path\":" << lhdc::json_string(utf8(detail->InfFileName))
            << ",\"section\":" << lhdc::json_string(utf8(detail->SectionName)) << "}\n";
    }
    bool install(SP_DRVINFO_DATA_W& driver,bool restore) {
        if (restore) {
            // The inbox SRC interface's MSEP properties are required for
            // AudioEndpointBuilder to expose the stereo playback endpoint.
            if (!SetupDiSetSelectedDriverW(set_,&data_,&driver)) win_error("Select inbox driver");
            if (!SetupDiCallClassInstaller(DIF_INSTALLINTERFACES,set_,&data_)) win_error("Register inbox audio interfaces");
        }
        BOOL reboot=FALSE;
        if (!DiInstallDevice(nullptr,set_,&data_,&driver,DIIDFLAG_NOFINISHINSTALLUI,&reboot)) win_error("Single-instance DiInstallDevice");
        return reboot!=FALSE;
    }
private:
    HDEVINFO set_=INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA data_{};
};
std::filesystem::path windows_inf() {
    std::vector<wchar_t> root(MAX_PATH);
    const auto size=GetWindowsDirectoryW(root.data(),static_cast<UINT>(root.size()));
    if (!size || size>=root.size()) win_error("Windows directory");
    return std::filesystem::path(root.data())/L"INF"/standard_inf;
}
int bind(const wchar_t* instance,const std::filesystem::path& supplied,const std::filesystem::path& certificate,bool restore) {
    Target target(instance);
    target.snapshot("before_binding");
    if (!administrator()) throw std::runtime_error("Run binding in an elevated administrator process");
    const auto current_service=target.property(DEVPKEY_Device_Service);
    if (restore) {
        const bool inbox=equal(current_service,L"BthA2dp") && equal(target.property(DEVPKEY_Device_DriverInfPath),standard_inf);
        if (!inbox && !equal(current_service,L"lhdc-transport")) throw std::runtime_error("Restore refused: current driver is not this project's transport or the recorded inbox driver");
        target.clear_transport_security();
        target.reset_inbox_access();
    } else {
        if (!(code_integrity_options()&CODEINTEGRITY_OPTION_TESTSIGN)) throw std::runtime_error("Test signing is not active in the running kernel; do not bind this test driver before rebooting into test mode");
        if (!equal(current_service,L"BthA2dp") || !equal(target.property(DEVPKEY_Device_DriverInfPath),standard_inf))
            throw std::runtime_error("Binding refused: expected the recorded Microsoft A2DP driver");
        if (!equal(supplied.filename().wstring(),L"lhdc-transport.inf")) throw std::invalid_argument("Only lhdc-transport.inf may be bound");
        lhdc::verify_package_signature(std::filesystem::absolute(supplied).parent_path(),certificate);
    }
    auto inf=std::filesystem::absolute(supplied);
    if (!restore) {
        wchar_t published[MAX_PATH]{};
        if (!SetupCopyOEMInfW(inf.c_str(),inf.parent_path().c_str(),SPOST_PATH,0,published,MAX_PATH,nullptr,nullptr)) win_error("Driver Store staging");
        std::cout << "{\"event\":\"driver_staged\",\"published_inf\":" << lhdc::json_string(utf8(published)) << "}\n";
        inf=published;
    }
    auto drivers=target.build_candidates(inf);
    if (drivers.size()!=1) throw std::runtime_error("Expected exactly one compatible driver in the selected INF");
    target.print_candidate(drivers[0]);
    const auto reboot=target.install(drivers[0],restore);
    target.snapshot("after_binding");
    const auto service=target.property(DEVPKEY_Device_Service);
    const auto active_inf=target.property(DEVPKEY_Device_DriverInfPath);
    const bool verified=equal(service,restore?L"BthA2dp":L"lhdc-transport") && equal(active_inf,inf.filename().wstring()) && target.problem()==0;
    std::cout << "{\"event\":\"binding_result\",\"reboot_required\":" << (reboot?"true":"false")
        << ",\"binding_verified\":" << (verified?"true":"false") << "}\n";
    if (reboot) return 3;
    if (!verified) throw std::runtime_error("Driver install returned success, but target service/INF/problem does not verify the binding");
    return 0;
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        if (argc<2) throw std::invalid_argument("Usage: lhdc-device readiness | verify-package DIRECTORY CERTIFICATE | snapshot INSTANCE | candidates INSTANCE INF | install INSTANCE INF CERTIFICATE | restore INSTANCE");
        const std::wstring command=argv[1];
        if (command==L"readiness" && argc==2) readiness();
        else if (command==L"verify-package" && argc==4) lhdc::verify_package_signature(argv[2],argv[3]);
        else if (command==L"snapshot" && argc==3) {Target target(argv[2]);target.snapshot("snapshot");}
        else if (command==L"candidates" && argc==4) {
            Target target(argv[2]);auto candidates=target.build_candidates(argv[3]);
            for (auto& candidate:candidates) target.print_candidate(candidate);
            std::cout << "{\"event\":\"candidate_count\",\"count\":" << candidates.size() << "}\n";
            if (candidates.size()!=1) return 1;
        } else if (command==L"install" && argc==5) return bind(argv[2],argv[3],argv[4],false);
        else if (command==L"restore" && argc==3) return bind(argv[2],windows_inf(),{},true);
        else throw std::invalid_argument("Unknown command or invalid arguments");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "{\"event\":\"error\",\"message\":" << lhdc::json_string(error.what()) << "}\n";
        return 1;
    }
}
