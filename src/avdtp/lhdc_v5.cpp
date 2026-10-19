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
        // Optional peer capabilities are omitted from our configuration. A
        // peer requiring content protection can reject SetConfiguration.
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
ConfigurationReadback verify_configuration(std::span<const std::uint8_t> expected,
    std::span<const std::uint8_t> actual,std::span<const std::uint8_t> peer_capabilities) {
    auto left=capabilities(expected),right=capabilities(actual);
    auto order=[](const Capability& a,const Capability& b){return a.category<b.category;};
    std::sort(left.begin(),left.end(),order); std::sort(right.begin(),right.end(),order);
    const auto same=[](const Capability& a,const Capability& b){return a.category==b.category && a.data==b.data;};
    if (std::equal(left.begin(),left.end(),right.begin(),right.end(),same)) return ConfigurationReadback::exact;

    // Some peers echo their advertised codec capabilities after accepting
    // SetConfiguration. Accept only that exact codec, with an optional empty
    // Delay Reporting item; this does not verify the peer's active PCM format.
    if (right.size()==3 && right.back().category==8 && right.back().data.empty()) right.pop_back();
    if (left.size()!=2 || right.size()!=2 || left[0].category!=1 || left[1].category!=7 ||
        right[0].category!=1 || right[1].category!=7 || !left[0].data.empty() || !right[0].data.empty())
        throw std::runtime_error("GetConfiguration differs from selected configuration");

    const auto selected=v5_capabilities(expected);
    const auto peer=v5_capabilities(peer_capabilities);
    if (selected.sample_rates.size()!=1 || selected.bit_depths.size()!=1 ||
        std::find(peer.sample_rates.begin(),peer.sample_rates.end(),selected.sample_rates[0])==peer.sample_rates.end() ||
        std::find(peer.bit_depths.begin(),peer.bit_depths.end(),selected.bit_depths[0])==peer.bit_depths.end() ||
        selected.min_kbps>selected.max_kbps || selected.min_kbps<peer.min_kbps || selected.max_kbps>peer.max_kbps)
        throw std::runtime_error("Selected configuration exceeds the advertised LHDC V5 capabilities");

    const auto advertised=capabilities(peer_capabilities);
    const auto codec=std::find_if(advertised.begin(),advertised.end(),[](const Capability& cap){return cap.category==7;});
    if (right[1].data!=codec->data) throw std::runtime_error("GetConfiguration differs from selected configuration");
    return ConfigurationReadback::capability_echo;
}
}
