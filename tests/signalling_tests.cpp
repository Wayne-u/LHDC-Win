#include "host/signalling.hpp"
#include <deque>
#include <iostream>
#include <stdexcept>
namespace av=lhdc::avdtp;
static void require(bool value,const char* message) {
    if(!value) throw std::runtime_error(message);
}
template<class F> static void rejects(F action,const char* message) {
    try { action(); } catch(const std::exception&) { return; }
    throw std::runtime_error(message);
}
struct Link {
    std::deque<av::Bytes> incoming;
    std::vector<av::Bytes> outgoing;
    std::vector<DWORD> waits;
    DWORD empty_error=ERROR_TIMEOUT;
    DWORD send_error=ERROR_SUCCESS;
    lhdc::Signalling protocol() {
        return {{48000,24,500},
            [this](auto bytes) {
                if(send_error!=ERROR_SUCCESS) throw lhdc::WindowsError("Fake signal write",send_error);
                outgoing.emplace_back(bytes.begin(),bytes.end());
            },
            [this](DWORD timeout) {
                waits.push_back(timeout);
                if(incoming.empty()) throw lhdc::WindowsError("Fake signal read",empty_error);
                auto bytes=std::move(incoming.front());incoming.pop_front();return bytes;
            }};
    }
};
int main() {
    try {
        {
            Link link;auto protocol=link.protocol();
            link.incoming={av::command(1,av::Signal::discover)};
            protocol.poll_peer(5);
            require(link.waits==std::vector<DWORD>{5},"Idle read must use its short timeout");
            require(link.outgoing==std::vector<av::Bytes>{av::unhex("12010400")},"Idle source must answer peer discovery");
            link.incoming={av::unhex("02010c08")};
            const auto endpoints=av::endpoints(protocol.accept(av::Signal::discover));
            require(endpoints.size()==1 && endpoints[0].seid==3,"Idle command must not consume the following transaction response");
            require(link.outgoing.back()==av::command(0,av::Signal::discover),"Idle replies must not advance local transaction labels");
        }
        {
            Link link;auto protocol=link.protocol();
            // Peer GetCapabilities fragmented across a short idle poll boundary.
            link.incoming={av::unhex("140202"),av::unhex("1c04")};
            protocol.poll_peer(5);
            auto expected=av::command(1,av::Signal::get_capabilities,av::local_capabilities({48000,24,500}));
            expected[0]|=2;
            require(link.outgoing==std::vector<av::Bytes>{expected},"Idle fragmented commands must be fully reassembled and answered");
            require(link.waits.size()==2 && link.waits[0]==5 && link.waits[1]>5,"A started message must finish before a configuration transaction can take over");
        }
        const DWORD timeouts[]={ERROR_TIMEOUT,ERROR_SEM_TIMEOUT};
        for(const auto error:timeouts) {
            Link link;link.empty_error=error;auto protocol=link.protocol();
            require(!protocol.poll_peer(5) && link.outgoing.empty(),"An initial empty-poll timeout must leave the session usable");
        }
        const DWORD errors[]={ERROR_DEVICE_NOT_CONNECTED,ERROR_OPERATION_ABORTED};
        for(const auto error:errors) {
            Link link;link.empty_error=error;auto protocol=link.protocol();
            bool unchanged=false;
            try { protocol.poll_peer(5); }
            catch(const lhdc::WindowsError& failure) { unchanged=failure.code==error; }
            require(unchanged,"Disconnect and cancellation must propagate instead of appearing as an empty poll");
        }
        {
            Link link;link.send_error=ERROR_TIMEOUT;auto protocol=link.protocol();
            link.incoming={av::command(1,av::Signal::discover)};
            bool failed=false;
            try { protocol.poll_peer(5); }
            catch(const lhdc::WindowsError& error) { failed=error.code==ERROR_TIMEOUT; }
            require(failed,"A failed peer acknowledgement must not be swallowed as a receive timeout");
        }
        {
            Link link;auto protocol=link.protocol();
            link.incoming={av::unhex("140202")};
            bool failed=false;
            try { protocol.poll_peer(5); }
            catch(const lhdc::WindowsError&) { throw std::runtime_error("Partial idle message was mistaken for a harmless empty poll"); }
            catch(const std::runtime_error&) { failed=true; }
            require(failed,"A partial-message timeout must invalidate the session");
        }
        {
            Link link;auto protocol=link.protocol();
            const auto selected=av::local_capabilities({48000,24,500});
            protocol.set_configuration(selected);
            link.incoming={av::command(2,av::Signal::get_configuration,av::Bytes{4}),
                av::command(3,av::Signal::discover),av::command(4,av::Signal::start,av::Bytes{4})};
            for(unsigned i=0;i<3;++i) require(protocol.poll_peer(5),"Prepared idle configuration must keep answering peer commands");
            auto readback=av::command(2,av::Signal::get_configuration,selected);readback[0]|=2;
            require(link.outgoing==std::vector<av::Bytes>{readback,av::unhex("32010600"),av::unhex("43070431")},
                "Prepared source must expose its configuration and occupied SEP without starting idle media");
            require(!protocol.peer_released(),"Readback, discovery and rejected Start must retain the prepared configuration");
        }
        for(const auto signal:{av::Signal::close,av::Signal::abort,av::Signal::suspend}) {
            Link link;auto protocol=link.protocol();
            protocol.set_configuration(av::local_capabilities({48000,24,500}));
            link.incoming={av::command(3,signal,av::Bytes{4})};
            rejects([&]{protocol.poll_peer(5);},"Peer stop must terminate the prepared session after acknowledgement");
            auto reply=av::command(3,signal);reply[0]|=2;
            require(link.outgoing==std::vector<av::Bytes>{reply},"Peer stop must be acknowledged");
            require(protocol.peer_released()==(signal!=av::Signal::suspend),"Only Close and Abort release the peer configuration");
        }
        {
            Link link;auto protocol=link.protocol();
            link.incoming={av::unhex("0201")};
            rejects([&]{protocol.poll_peer(5);},"An unsolicited response cannot be reused as a later Accept");
        }
        {
            Link link;auto protocol=link.protocol();
            link.incoming={av::command(4,av::Signal::discover),av::unhex("0602010c"),av::unhex("0e08")};
            const auto endpoints=av::endpoints(protocol.accept(av::Signal::discover));
            require(endpoints.size()==1 && endpoints[0].seid==3,"A transaction must handle peer commands and fragmented Accepts in order");
            require(link.outgoing.size()==2 && link.outgoing[1]==av::unhex("42010400"),"Peer command during a transaction must receive a reply");
        }
        std::cout<<"Idle signalling, fragment handoff, timeout and transaction checks passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
