#include "protocol.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
namespace lhdc::avrcp {
Frame parse(std::span<const std::uint8_t> bytes) {
    if(bytes.size()<6 || (bytes[0]&0x0d) || bytes[1]!=0x11 || bytes[2]!=0x0e ||
       (bytes[3]&0xf0) || (bytes[4]!=0x48 && bytes[4]!=0xff))
        throw std::runtime_error("Invalid or fragmented AVRCP control frame");
    return {static_cast<std::uint8_t>(bytes[0]>>4),bytes[3],bytes[5],(bytes[0]&2)!=0,Bytes(bytes.begin()+6,bytes.end())};
}
Vendor vendor(const Frame& frame) {
    const auto& bytes=frame.operands;
    if(frame.opcode!=0 || bytes.size()<7 || bytes[0]!=0 || bytes[1]!=0x19 || bytes[2]!=0x58 || bytes[4]!=0 ||
       bytes.size()!=7+static_cast<std::size_t>((bytes[5]<<8)|bytes[6]))
        throw std::runtime_error("Invalid AVRCP vendor PDU");
    return {bytes[3],Bytes(bytes.begin()+7,bytes.end())};
}
Bytes command(std::uint8_t label,std::uint8_t ctype,std::uint8_t pdu,std::span<const std::uint8_t> parameters) {
    if(label>15 || (ctype!=0 && ctype!=1 && ctype!=3) || parameters.size()>0xffff)
        throw std::invalid_argument("Invalid AVRCP command");
    Bytes bytes={static_cast<std::uint8_t>(label<<4),0x11,0x0e,ctype,0x48,0,0,0x19,0x58,pdu,0,
        static_cast<std::uint8_t>(parameters.size()>>8),static_cast<std::uint8_t>(parameters.size())};
    bytes.insert(bytes.end(),parameters.begin(),parameters.end());return bytes;
}
Bytes capabilities(std::uint8_t label) { const std::uint8_t parameters[]={3}; return command(label,1,get_capabilities,parameters); }
Bytes notification(std::uint8_t label) { const std::uint8_t parameters[]={volume_changed,0,0,0,0}; return command(label,3,register_notification,parameters); }
Bytes volume(std::uint8_t label,std::uint8_t value) {
    if(value>127) throw std::invalid_argument("AVRCP volume must be 0..127");
    return command(label,0,set_absolute_volume,std::span(&value,1));
}
Bytes reply(const Frame& request,std::uint8_t ctype,std::span<const std::uint8_t> operands) {
    if(request.response || ctype<8 || ctype>15) throw std::invalid_argument("Invalid AVRCP response");
    const auto subunit=static_cast<std::uint8_t>(request.opcode==0x30 || request.opcode==0x31 ? 0xff : 0x48);
    Bytes bytes={static_cast<std::uint8_t>((request.label<<4)|2),0x11,0x0e,ctype,subunit,request.opcode};
    bytes.insert(bytes.end(),operands.begin(),operands.end());return bytes;
}
bool supports_volume(const Frame& frame) {
    const auto pdu=vendor(frame);
    if(!frame.response || frame.ctype!=stable || pdu.pdu!=get_capabilities || pdu.parameters.size()<2 ||
        pdu.parameters[0]!=3 || pdu.parameters.size()!=2u+pdu.parameters[1])
        throw std::runtime_error("Invalid AVRCP event capabilities response");
    return std::find(pdu.parameters.begin()+2,pdu.parameters.end(),volume_changed)!=pdu.parameters.end();
}
std::uint8_t volume_value(const Frame& frame) {
    const auto pdu=vendor(frame);
    if(!frame.response) throw std::runtime_error("Expected AVRCP volume response");
    if(pdu.pdu==set_absolute_volume && frame.ctype==accepted && pdu.parameters.size()==1)
        return pdu.parameters[0]&0x7f;
    if(pdu.pdu==register_notification && (frame.ctype==interim || frame.ctype==changed) &&
        pdu.parameters.size()==2 && pdu.parameters[0]==volume_changed)
        return pdu.parameters[1]&0x7f;
    throw std::runtime_error("Rejected or invalid AVRCP volume response");
}
std::uint8_t absolute_volume(float scalar) {
    if(!std::isfinite(scalar) || scalar<0 || scalar>1) throw std::invalid_argument("Volume scalar must be 0..1");
    return static_cast<std::uint8_t>(std::lround(scalar*127));
}
}
