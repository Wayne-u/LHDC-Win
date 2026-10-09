#include "lhdc_v5.hpp"
#include <stdexcept>
#include <algorithm>
namespace lhdc::avdtp {
static constexpr std::uint32_t minimums[]{64,160,256,400};
static constexpr std::uint32_t maximums[]{1000,400,500,900};
Bytes local_capabilities() { return local_capabilities(Profile{}); }
Bytes local_capabilities(const Profile& profile) {
    quality_index(profile);
    const auto rate=profile.sample_rate==44100?0x20:profile.sample_rate==48000?0x10:profile.sample_rate==96000?0x04:0x01;
    unsigned minimum=0,maximum=0;
    for (unsigned i=0;i<4;++i) if (minimums[i]<=profile.kbps) minimum=i;
    for (unsigned i=1;i<4;++i)
        if (maximums[i]>=profile.kbps && maximums[i]<maximums[maximum]) maximum=i;
    const auto depth=profile.bits==16?0x04:0x02;
    // Bitrate limits are coarse protocol categories; the exact target is set in the encoder.
    return {1,0,7,13,0,0xff,0x3a,0x05,0,0,0x35,0x4c,static_cast<std::uint8_t>(rate),
        static_cast<std::uint8_t>((minimum<<6)|(maximum<<4)|depth),0x11,0,0};
}
V5Capabilities v5_capabilities(std::span<const std::uint8_t> raw) {
    const auto caps=capabilities(raw);
    bool transport=false,codec=false;
    V5Capabilities result{};
    for (const auto& cap : caps) {
        if (cap.category==4) throw std::runtime_error("Content Protection is not supported by this probe");
        if (cap.category==1) {
            if (transport || !cap.data.empty()) throw std::runtime_error("Invalid Media Transport capability");
            transport=true;
        }
        if (cap.category!=7) continue;
        if (codec) throw std::runtime_error("Multiple media codecs in one SEP");
        codec=true;
        const auto vendor=vendor_codec(cap);
        if (!vendor || vendor->vendor_id!=lhdc_v5_vendor || vendor->codec_id!=lhdc_v5_codec)
            throw std::runtime_error("SEP does not advertise LHDC V5");
        if (vendor->data.size()!=5) throw std::runtime_error("Unexpected LHDC V5 capability length");
        const auto& data=vendor->data;
        if ((data[2]&0x11)!=0x11) throw std::runtime_error("Peer does not support version1/5ms");
        for (const auto [mask,rate] : {std::pair{0x20,44100u},{0x10,48000u},{0x04,96000u},{0x01,192000u}})
            if (data[0]&mask) result.sample_rates.push_back(rate);
        if (data[1]&0x04) result.bit_depths.push_back(16);
        if (data[1]&0x02) result.bit_depths.push_back(24);
        result.min_kbps=minimums[(data[1]>>6)&3]; result.max_kbps=maximums[(data[1]>>4)&3];
    }
    if (!transport || !codec) throw std::runtime_error("Missing transport or codec capability");
    return result;
}
Bytes select_configuration(std::span<const std::uint8_t> raw,const Profile& profile) {
    const auto peer=v5_capabilities(raw);
    quality_index(profile);
    if (std::find(peer.sample_rates.begin(),peer.sample_rates.end(),profile.sample_rate)==peer.sample_rates.end() ||
        std::find(peer.bit_depths.begin(),peer.bit_depths.end(),profile.bits)==peer.bit_depths.end() ||
        profile.kbps<peer.min_kbps || profile.kbps>peer.max_kbps)
        throw std::runtime_error("Requested profile exceeds peer LHDC V5 capabilities");
    return local_capabilities(profile);
}
void verify_configuration(std::span<const std::uint8_t> expected,std::span<const std::uint8_t> actual) {
    auto left=capabilities(expected),right=capabilities(actual);
    auto order=[](const Capability& a,const Capability& b){return a.category<b.category;};
    std::sort(left.begin(),left.end(),order); std::sort(right.begin(),right.end(),order);
    if (left.size()!=right.size()) throw std::runtime_error("GetConfiguration differs from selected configuration");
    for (std::size_t i=0;i<left.size();++i)
        if (left[i].category!=right[i].category || left[i].data!=right[i].data)
            throw std::runtime_error("GetConfiguration differs from selected configuration");
}
}
