#include "control/hey_melody.hpp"
#include <iostream>
#include <stdexcept>
namespace hey=lhdc::hey_melody;
static void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }
template<class F> static void rejects(F action) {
    try { action(); } catch(const std::exception&) { return; }
    throw std::runtime_error("Expected protocol rejection");
}
int main() {
    try {
        // Captured response from Enco X4; split TCP/RFCOMM reads and notifications.
        const hey::Bytes status{0xaa,0x0b,0,0,0x0d,0x81,3,4,0,0,1,0x18,1};
        hey::Framer framer;
        for(std::size_t i=0;i<status.size();++i) {
            framer.append(std::span(status).subspan(i,1));
            const auto frame=framer.next();
            require(frame.has_value()==(i+1==status.size()),"Fragmented receive boundaries");
            if(frame) require(frame->command==0x810d && frame->sequence==3 && hey::hi_res_status(frame->payload)==true,"Captured Hi-Res response");
        }
        hey::Bytes combined{0,0x21};
        const auto notification=hey::encode({0x0204,0xff,hey::Bytes(180,0)});
        combined.insert(combined.end(),notification.begin(),notification.end());
        combined.insert(combined.end(),status.begin(),status.end());
        framer.append(combined);
        require(framer.next()->payload.size()==180,"Varint frame length");
        require(framer.next()->command==0x810d && !framer.next(),"Coalesced response after notification");
        auto malformed=status;malformed[7]=5;framer.append(malformed);
        rejects([&]{framer.next();});
        require(!hey::hi_res_status(hey::Bytes{0,1,0x18,0xff}).has_value(),"Unknown status is not disabled");
        rejects([]{hey::hi_res_status(hey::Bytes{0,2,0x18,1});});
        rejects([]{hey::hi_res_status(hey::Bytes{1,0});});
        unsigned reads=0,writes=0;
        auto request=[&](std::uint16_t cmd,std::span<const std::uint8_t> payload) {
            if(cmd==0x010d) { require(hey::Bytes(payload.begin(),payload.end())==hey::Bytes{1,0x18},"Only Hi-Res is queried");return hey::Bytes{0,1,0x18,static_cast<std::uint8_t>(reads++?1:0)}; }
            require(cmd==0x0403 && hey::Bytes(payload.begin(),payload.end())==hey::Bytes{0x18,1},"Only Hi-Res is changed");++writes;return hey::Bytes{0};
        };
        const auto enabled=hey::hi_res(request,true);
        require(enabled.before==false && enabled.enabled==true && enabled.changed && reads==2 && writes==1,"Write ACK and readback");
        writes=0;
        const auto unchanged=hey::hi_res(request,true);
        require(!unchanged.changed && writes==0,"No write when already enabled");
        rejects([]{hey::hi_res([](auto,auto){return hey::Bytes{0,0};},true);});
        unsigned steps=0;
        rejects([&]{hey::hi_res([&](auto cmd,auto){ ++steps;return cmd==0x0403?hey::Bytes{0}:hey::Bytes{0,1,0x18,0}; },true);});
        require(steps==3,"ACK alone cannot prove success");
        std::cout<<"Earbud control checks passed\n";
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
