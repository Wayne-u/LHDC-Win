#include <winsock2.h>
#include <ws2bth.h>
#include "rfcomm.hpp"
#include "hey_melody.hpp"
#include "transport/windows.hpp"
#include "codec/profile.hpp"
#include "host/json.hpp"
#include <chrono>
#include <iostream>
namespace lhdc {
namespace {
namespace hey=hey_melody;
using Clock=std::chrono::steady_clock;
class SocketError : public std::runtime_error {
public:
    SocketError(const char* operation,int value):std::runtime_error(std::string(operation)+" failed, WSA="+std::to_string(value)),code(value){}
    int code;
};
void socket_error(const char* operation,int error=WSAGetLastError()) {
    throw SocketError(operation,error);
}
class Winsock {
public:
    Winsock() { WSADATA data{};const auto error=WSAStartup(MAKEWORD(2,2),&data);if(error) socket_error("Winsock startup",error); }
    ~Winsock() { WSACleanup(); }
};
class Rfcomm {
public:
    explicit Rfcomm(std::uint64_t address) {
        const GUID services[]{
            {0x0000079a,0xd102,0x11e1,{0x9b,0x23,0x00,0x02,0x5b,0x00,0xa5,0xa5}},
            {0x00001107,0xd102,0x11e1,{0x9b,0x23,0x00,0x02,0x5b,0x00,0xa5,0xa5}}};
        int last_error=WSAEADDRNOTAVAIL;
        for(const auto& service:services) {
            const auto candidate=socket(AF_BTH,SOCK_STREAM,BTHPROTO_RFCOMM);
            if(candidate==INVALID_SOCKET) socket_error("RFCOMM socket");
            try {
                u_long nonblocking=1;
                if(ioctlsocket(candidate,FIONBIO,&nonblocking)) socket_error("RFCOMM nonblocking mode");
                SOCKADDR_BTH peer{};peer.addressFamily=AF_BTH;peer.btAddr=address;peer.serviceClassId=service;
                if(connect(candidate,reinterpret_cast<sockaddr*>(&peer),sizeof(peer))==SOCKET_ERROR) {
                    const auto error=WSAGetLastError();
                    if(error!=WSAEWOULDBLOCK) socket_error("HeyMelody service connect",error);
                    wait(candidate,true,Clock::now()+std::chrono::seconds(5),"HeyMelody connect");
                }
                int status=0,size=sizeof(status);
                if(getsockopt(candidate,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&status),&size)) socket_error("RFCOMM connection status");
                if(status) socket_error("HeyMelody service connect",status);
                socket_=candidate;return;
            } catch(const SocketError& error) {
                last_error=error.code;closesocket(candidate);
                // SDP may expose only one of the two known service UUIDs.
                if(last_error!=WSAEADDRNOTAVAIL && last_error!=WSASERVICE_NOT_FOUND) throw;
            } catch(...) {
                closesocket(candidate);throw;
            }
        }
        socket_error("HeyMelody service unavailable",last_error);
    }
    ~Rfcomm() { if(socket_!=INVALID_SOCKET) closesocket(socket_); }
    Rfcomm(const Rfcomm&)=delete;
    Rfcomm& operator=(const Rfcomm&)=delete;
    hey::Bytes request(std::uint16_t command,std::span<const std::uint8_t> payload={}) {
        const auto sequence=sequence_++;
        const auto packet=hey::encode({command,sequence,hey::Bytes(payload.begin(),payload.end())});
        const auto deadline=Clock::now()+std::chrono::seconds(5);
        const auto operation="HeyMelody command "+std::to_string(command);
        std::size_t sent=0;
        while(sent<packet.size()) {
            wait(socket_,true,deadline,operation.c_str());
            const auto count=send(socket_,reinterpret_cast<const char*>(packet.data()+sent),static_cast<int>(packet.size()-sent),0);
            if(count==SOCKET_ERROR) { if(WSAGetLastError()==WSAEWOULDBLOCK) continue;socket_error("HeyMelody send"); }
            if(!count) throw std::runtime_error("HeyMelody control channel closed during send");
            sent+=static_cast<std::size_t>(count);
        }
        for(;;) {
            while(const auto frame=framer_.next()) {
                if(frame->command==(command|0x8000) && frame->sequence==sequence) return frame->payload;
            }
            wait(socket_,false,deadline,operation.c_str());
            std::uint8_t buffer[512];
            const auto count=recv(socket_,reinterpret_cast<char*>(buffer),sizeof(buffer),0);
            if(count==SOCKET_ERROR) { if(WSAGetLastError()==WSAEWOULDBLOCK) continue;socket_error("HeyMelody receive"); }
            if(!count) throw std::runtime_error("HeyMelody control channel closed before response");
            framer_.append(std::span(buffer,static_cast<std::size_t>(count)));
        }
    }
private:
    static void wait(SOCKET socket,bool writing,Clock::time_point deadline,const char* operation) {
        const auto remaining=std::chrono::duration_cast<std::chrono::microseconds>(deadline-Clock::now()).count();
        if(remaining<=0) throw std::runtime_error(std::string(operation)+(writing?" send timed out":" response timed out"));
        fd_set ready{},errors{};FD_SET(socket,&ready);FD_SET(socket,&errors);
        timeval timeout{static_cast<long>(remaining/1000000),static_cast<long>(remaining%1000000)};
        const auto result=select(0,writing?nullptr:&ready,writing?&ready:nullptr,&errors,&timeout);
        if(result==SOCKET_ERROR) socket_error("RFCOMM wait");
        if(!result) throw std::runtime_error(std::string(operation)+(writing?" send timed out":" response timed out"));
        if(FD_ISSET(socket,&errors)) {
            int error=0,size=sizeof(error);
            if(getsockopt(socket,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&size)) socket_error("RFCOMM error status");
            WSASetLastError(error);socket_error("HeyMelody control channel",error);
        }
    }
    Winsock winsock_;
    SOCKET socket_=INVALID_SOCKET;
    std::uint8_t sequence_=1;
    hey::Framer framer_;
};
void request_capability_refresh() {
    const auto revision=capability_revision()+1;
    HKEY key=nullptr;
    const auto opened=RegOpenKeyExW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\LHDC-Win",0,KEY_SET_VALUE,&key);
    if(opened==ERROR_FILE_NOT_FOUND) return; // No LHDC installation/cache yet.
    if(opened!=ERROR_SUCCESS) throw WindowsError("Hi-Res capability refresh access",opened);
    auto error=RegSetValueExW(key,L"PeerCapabilities",0,REG_BINARY,nullptr,0);
    if(error==ERROR_SUCCESS) error=RegSetValueExW(key,L"CapabilityRevision",0,REG_DWORD,reinterpret_cast<const BYTE*>(&revision),sizeof(revision));
    RegCloseKey(key);
    if(error!=ERROR_SUCCESS) throw WindowsError("Hi-Res changed but capability refresh request",error);
}
}
std::uint32_t capability_revision() {
    DWORD value=0,size=sizeof(value);
    const auto error=RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\LHDC-Win",L"CapabilityRevision",RRF_RT_REG_DWORD,nullptr,&value,&size);
    if(error!=ERROR_SUCCESS && error!=ERROR_FILE_NOT_FOUND) throw WindowsError("Capability revision read",error);
    return value;
}
void inspect_hi_res(std::optional<bool> desired) {
    const auto address=target_device().address;
    if(!bluetooth_connected(address)) throw std::runtime_error("Connect Enco X4 before querying Hi-Res");
    if(desired==false) {
        Profile profile{};DWORD size=sizeof(profile);
        const auto error=RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\LHDC-Win",L"Profile",RRF_RT_REG_BINARY,nullptr,&profile,&size);
        if(error!=ERROR_SUCCESS && error!=ERROR_FILE_NOT_FOUND) throw WindowsError("Hi-Res profile guard",error);
        if(error==ERROR_SUCCESS && (size!=sizeof(profile) || profile.sample_rate>48000 || profile.kbps>400))
            throw std::runtime_error("Apply a LHDC profile at or below 48 kHz / 400 kbps before disabling Hi-Res");
    }
    Rfcomm control(address);
    const auto handshake=control.request(0x0100);
    if(handshake.empty() || handshake[0]!=0) throw std::runtime_error("Earbud rejected HeyMelody handshake");
    const auto result=hey_melody::hi_res([&](auto cmd,auto payload){return control.request(cmd,payload);},desired);
    if(result.changed) request_capability_refresh();
    const auto boolean=[](std::optional<bool> value){return value.has_value()?(*value?"true":"false"):"null";};
    std::cout<<"{\"event\":\"hi_res\",\"supported\":"<<(result.enabled.has_value()?"true":"false")
        <<",\"enabled\":"<<boolean(result.enabled)<<",\"before\":"<<boolean(result.before)<<",\"changed\":"<<(result.changed?"true":"false")<<"}\n";
}
}
