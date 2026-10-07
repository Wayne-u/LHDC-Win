#include "packetizer.hpp"
#include <stdexcept>
namespace lhdc {
Packetizer::Packetizer(std::uint32_t mtu,std::uint32_t samples,std::uint16_t sequence,
    std::uint32_t timestamp,std::uint8_t media_sequence)
    :mtu_(mtu),block_samples_(samples),timestamp_(timestamp),sequence_(sequence),media_sequence_(media_sequence) {
    if (mtu<=media_header_bytes || !samples) throw std::invalid_argument("Invalid media MTU or block duration");
}
std::vector<std::uint8_t> Packetizer::pack(const EncodedPacket& packet) {
    if (packet.payload.empty() && packet.frames==0) return {};
    if (packet.payload.empty() || !packet.frames || packet.frames>63 || packet.payload.size()>mtu_-media_header_bytes)
        throw std::invalid_argument("Invalid LHDC frame count or media packet exceeds MTU");
    // RTP V2 / dynamic payload type 96 / fixed SSRC 1, then LHDC count and sequence.
    std::vector<std::uint8_t> result{0x80,0x60,static_cast<std::uint8_t>(sequence_>>8),static_cast<std::uint8_t>(sequence_),
        static_cast<std::uint8_t>(timestamp_>>24),static_cast<std::uint8_t>(timestamp_>>16),
        static_cast<std::uint8_t>(timestamp_>>8),static_cast<std::uint8_t>(timestamp_),0,0,0,1,
        static_cast<std::uint8_t>(packet.frames<<2),media_sequence_};
    result.insert(result.end(),packet.payload.begin(),packet.payload.end());
    ++sequence_; ++media_sequence_; timestamp_+=packet.frames*block_samples_;
    return result;
}
}
