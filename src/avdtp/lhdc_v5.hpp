#pragma once
#include "protocol.hpp"
#include "codec/profile.hpp"
namespace lhdc::avdtp {
inline constexpr std::uint32_t lhdc_v5_vendor=0x0000053a;
inline constexpr std::uint16_t lhdc_v5_codec=0x4c35;
Bytes local_capabilities();
Bytes local_capabilities(const Profile& profile);
struct V5Capabilities {
    std::vector<std::uint32_t> sample_rates;
    std::vector<std::uint32_t> bit_depths;
    std::uint32_t min_kbps, max_kbps;
};
V5Capabilities v5_capabilities(std::span<const std::uint8_t> raw);
Bytes select_configuration(std::span<const std::uint8_t> peer_capabilities,const Profile& profile);
void verify_configuration(std::span<const std::uint8_t> expected,std::span<const std::uint8_t> actual);
}
