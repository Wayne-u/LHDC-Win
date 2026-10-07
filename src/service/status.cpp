#include "status.hpp"
#include "avdtp/protocol.hpp"
#include "host/json.hpp"
#include <windows.h>
#include <iostream>
#include <stdexcept>
#include <string>
namespace lhdc {
void inspect_audio_service() {
    const auto manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
    if (!manager) throw std::runtime_error("SCM status open failed, Win32="+std::to_string(GetLastError()));
    const auto service=OpenServiceW(manager,L"LHDC-Win",SERVICE_QUERY_STATUS);
    const auto error=GetLastError(); CloseServiceHandle(manager);
    if (!service) {
        if (error==ERROR_SERVICE_DOES_NOT_EXIST) { std::cout << "{\"event\":\"audio_service\",\"installed\":false,\"running\":false}\n"; return; }
        throw std::runtime_error("Audio service status open failed, Win32="+std::to_string(error));
    }
    SERVICE_STATUS_PROCESS status{}; DWORD size=0;
    const auto ok=QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,reinterpret_cast<BYTE*>(&status),sizeof(status),&size);
    const auto query_error=GetLastError(); CloseServiceHandle(service);
    if (!ok) throw std::runtime_error("Audio service status query failed, Win32="+std::to_string(query_error));
    HKEY profile=nullptr;
    const auto writable=RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\LHDC-Win",0,KEY_SET_VALUE,&profile);
    if (writable==ERROR_SUCCESS) RegCloseKey(profile);
    else if (writable!=ERROR_ACCESS_DENIED && writable!=ERROR_FILE_NOT_FOUND) throw std::runtime_error("Codec profile access query failed, Win32="+std::to_string(writable));
    avdtp::Bytes caps(4096); DWORD caps_size=static_cast<DWORD>(caps.size());
    const auto cached=RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\LHDC-Win",L"PeerCapabilities",RRF_RT_REG_BINARY,nullptr,caps.data(),&caps_size);
    if (cached==ERROR_SUCCESS) caps.resize(caps_size);
    else if (cached==ERROR_FILE_NOT_FOUND) caps.clear();
    else throw std::runtime_error("Peer capability cache read failed, Win32="+std::to_string(cached));
    std::cout << "{\"event\":\"audio_service\",\"installed\":true,\"running\":" << (status.dwCurrentState==SERVICE_RUNNING?"true":"false")
        << ",\"state\":" << status.dwCurrentState << ",\"pid\":" << status.dwProcessId << ",\"profile_editable\":" << (writable==ERROR_SUCCESS?"true":"false")
        << ",\"peer_capabilities\":" << json_string(avdtp::hex(caps)) << "}\n";
}
}
