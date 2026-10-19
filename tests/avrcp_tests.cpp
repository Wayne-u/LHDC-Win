#include "avrcp/protocol.hpp"
#include "audio/volume_memory.hpp"
#include <limits>
#include "avdtp/protocol.hpp"
#include <iostream>
#include <stdexcept>
#include <limits>
namespace av=lhdc::avrcp;
static void require(bool value) { if(!value) throw std::runtime_error("AVRCP check failed"); }
template<class F> static void rejects(F action) { bool rejected=false;try{action();}catch(const std::exception&){rejected=true;}require(rejected); }
int main() {
    try {
        require(av::capabilities(1)==lhdc::avdtp::unhex("10110e0148000019581000000103"));
        require(av::notification(2)==lhdc::avdtp::unhex("20110e034800001958310000050d00000000"));
        lhdc::VolumeMemory memory{1,0.31f,1,1};
        require(lhdc::restore_volume_memory(memory,true,false));
        require(!lhdc::restore_volume_memory(memory,false,false));
        require(!lhdc::restore_volume_memory(memory,true,true));
        memory.absolute=0;require(lhdc::restore_volume_memory(memory,false,false));
        memory.muted=2;require(!lhdc::valid_volume_memory(memory));memory.muted=0;
        memory.scalar=std::numeric_limits<float>::quiet_NaN();require(!lhdc::valid_volume_memory(memory));
        memory.scalar=1.1f;require(!lhdc::valid_volume_memory(memory));
        memory.scalar=0;memory.version=2;require(!lhdc::valid_volume_memory(memory));
        const auto caps=av::parse(lhdc::avdtp::unhex("12110e0c4800001958100000040302060d"));
        require(caps.label==1 && av::supports_volume(caps));
        const auto initial=av::parse(lhdc::avdtp::unhex("22110e0f4800001958310000020d22"));
        require(initial.label==2 && av::volume_value(initial)==34);
        require(av::volume(3,28)==lhdc::avdtp::unhex("30110e004800001958500000011c"));
        require(av::volume_value(av::parse(lhdc::avdtp::unhex("32110e094800001958500000011c")))==28);
        require(av::volume_value(av::parse(lhdc::avdtp::unhex("22110e0d4800001958310000020d00")))==0);
        require(av::volume_value(av::parse(lhdc::avdtp::unhex("22110e0d4800001958310000020dff")))==127); // RFD bit ignored
        const auto press=av::parse(lhdc::avdtp::unhex("10110e00487c4400"));
        const auto release=av::parse(lhdc::avdtp::unhex("10110e00487cc400"));
        require(av::media_button(press)->command==av::MediaCommand::play && av::media_button(press)->pressed);
        require(!av::media_button(release)->pressed);
        require(av::reply(press,av::accepted,press.operands)==lhdc::avdtp::unhex("12110e09487c4400"));
        require(!av::media_button(av::parse(lhdc::avdtp::unhex("10110e00487c7e00"))));
        require(!av::media_button(av::parse(lhdc::avdtp::unhex("12110e09487c4400"))));
        rejects([]{av::media_button(av::parse(lhdc::avdtp::unhex("10110e00487c44")));});
        rejects([]{av::media_button(av::parse(lhdc::avdtp::unhex("10110e00487c440101")));});
        rejects([]{av::volume_value(av::parse(lhdc::avdtp::unhex("32110e0a48000019585000000101")));});
        rejects([]{av::supports_volume(av::parse(lhdc::avdtp::unhex("12110e0c4800001958100000040303060d")));});
        require(!av::supports_volume(av::parse(lhdc::avdtp::unhex("12110e0c4800001958100000020300"))));
        const auto packet=lhdc::avdtp::unhex("22110e0f4800001958310000020d22");
        for(std::size_t size=0;size<packet.size();++size) rejects([&]{av::volume_value(av::parse(std::span(packet).first(size)));});
        for(unsigned byte:{1u,4u,8u}) {auto invalid=packet;invalid[0]|=static_cast<std::uint8_t>(byte);rejects([&]{av::parse(invalid);});}
        auto invalid=packet;invalid[2]=0x0f;rejects([&]{av::parse(invalid);});
        invalid=packet;invalid[6]=1;rejects([&]{av::vendor(av::parse(invalid));});
        invalid=packet;invalid[10]=1;rejects([&]{av::vendor(av::parse(invalid));});
        rejects([]{av::volume(0,128);}); rejects([]{av::capabilities(16);});
        require(av::absolute_volume(0)==0 && av::absolute_volume(1)==127 && av::absolute_volume(.5f)==64);
        for(unsigned value=0;value<128;++value) require(av::absolute_volume(value/127.0f)==value);
        rejects([]{av::absolute_volume(std::numeric_limits<float>::quiet_NaN());});
        rejects([]{av::absolute_volume(-.01f);}); rejects([]{av::absolute_volume(1.01f);});
        std::cout<<"AVRCP wire vectors and volume boundaries passed\n";
        return 0;
    } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
