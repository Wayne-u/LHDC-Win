#include "hey_melody.hpp"
#include <algorithm>
#include <stdexcept>
namespace lhdc::hey_melody {
Bytes encode(const Frame& frame) {
    if(frame.payload.size()>65535) throw std::invalid_argument("HeyMelody payload is too large");
    Bytes result{0xaa};
    auto length=frame.payload.size()+7;
    do {
        auto byte=static_cast<std::uint8_t>(length&0x7f);length>>=7;
        result.push_back(static_cast<std::uint8_t>(byte|(length?0x80:0)));
    } while(length);
    result.insert(result.end(),{0,0,static_cast<std::uint8_t>(frame.command),static_cast<std::uint8_t>(frame.command>>8),frame.sequence,
        static_cast<std::uint8_t>(frame.payload.size()),static_cast<std::uint8_t>(frame.payload.size()>>8)});
    result.insert(result.end(),frame.payload.begin(),frame.payload.end());
    return result;
}
void Framer::append(std::span<const std::uint8_t> bytes) {
    if(pending_.size()+bytes.size()>66000) throw std::runtime_error("HeyMelody receive buffer exceeded frame limit");
    pending_.insert(pending_.end(),bytes.begin(),bytes.end());
}
std::optional<Frame> Framer::next() {
    const auto start=std::find(pending_.begin(),pending_.end(),0xaa);
    pending_.erase(pending_.begin(),start);
    if(pending_.size()<2) return {};
    std::size_t offset=1,length=0;
    for(unsigned shift=0;;shift+=7) {
        if(offset==pending_.size()) return {};
        if(shift>14) throw std::runtime_error("Invalid HeyMelody frame length");
        const auto byte=pending_[offset++];length|=std::size_t(byte&0x7f)<<shift;
        if(!(byte&0x80)) break;
    }
    if(length<7 || length>65542) throw std::runtime_error("Invalid HeyMelody frame size");
    if(pending_.size()<offset+length) return {};
    if(pending_[offset]!=0 || pending_[offset+1]!=0) throw std::runtime_error("Fragmented HeyMelody link frame is unsupported");
    const auto payload_size=std::size_t(pending_[offset+5])|(std::size_t(pending_[offset+6])<<8);
    if(payload_size+7!=length) throw std::runtime_error("HeyMelody payload length mismatch");
    Frame frame{static_cast<std::uint16_t>(pending_[offset+2]|(pending_[offset+3]<<8)),pending_[offset+4],
        Bytes(pending_.begin()+offset+7,pending_.begin()+offset+length)};
    pending_.erase(pending_.begin(),pending_.begin()+offset+length);
    return frame;
}
std::optional<bool> hi_res_status(std::span<const std::uint8_t> payload) {
    if(payload.size()<2 || payload[0]!=0) throw std::runtime_error("Earbud rejected feature status query");
    if(payload.size()!=2+std::size_t(payload[1])*2) throw std::runtime_error("Invalid HeyMelody feature status response");
    std::optional<bool> result;
    for(std::size_t i=2;i<payload.size();i+=2) {
        if(payload[i]!=0x18) continue;
        if(payload[i+1]>1) return {};
        if(result.has_value()) throw std::runtime_error("Duplicate Hi-Res feature status");
        result=payload[i+1]==1;
    }
    return result;
}
HiResResult hi_res(const Request& request,std::optional<bool> desired) {
    const Bytes query{1,0x18};
    HiResResult result;
    result.before=result.enabled=hi_res_status(request(0x010d,query));
    if(!desired.has_value()) return result;
    if(!result.enabled.has_value()) throw std::runtime_error("Earbud did not report a supported Hi-Res switch");
    if(result.enabled==desired) return result;
    const Bytes write{0x18,static_cast<std::uint8_t>(*desired?1:0)};
    const auto response=request(0x0403,write);
    if(response!=Bytes{0}) throw std::runtime_error("Earbud rejected Hi-Res switch");
    result.enabled=hi_res_status(request(0x010d,query));
    if(result.enabled!=desired) throw std::runtime_error("Hi-Res switch readback did not match requested state");
    result.changed=true;
    return result;
}
}
