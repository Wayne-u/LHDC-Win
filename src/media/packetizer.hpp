#pragma once
#include "codec/encoder.hpp"
namespace lhdc {
inline constexpr std::uint32_t media_header_bytes=14;
class Packetizer {
public:
    Packetizer(std::uint32_t out_mtu,std::uint32_t block_samples,std::uint16_t sequence=0,
        std::uint32_t timestamp=0,std::uint8_t media_sequence=0);
    std::vector<std::uint8_t> pack(const EncodedPacket& packet);
private:
    std::uint32_t mtu_,block_samples_,timestamp_;
    std::uint16_t sequence_;
    std::uint8_t media_sequence_;
};
}
