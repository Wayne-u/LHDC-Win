#include "profile.hpp"
#include <algorithm>
#include <stdexcept>
namespace lhdc {
std::vector<std::uint32_t> bitrates(std::uint32_t rate) {
    if (rate!=44100 && rate!=48000 && rate!=96000 && rate!=192000)
        throw std::invalid_argument("Unsupported LHDC V5 sample rate");
    // Exact entries in the pinned encoder's table; 44.1k uses 240/480.
    return {64,160,192,rate==44100?240u:256u,320,400,rate==44100?480u:500u,900,1000};
}
std::uint32_t quality_index(const Profile& profile) {
    if (profile.bits!=16 && profile.bits!=24) throw std::invalid_argument("PCM must be 16 or 24 bit");
    const auto choices=bitrates(profile.sample_rate);
    const auto entry=std::find(choices.begin(),choices.end(),profile.kbps);
    if (entry==choices.end()) throw std::invalid_argument("Bitrate is not an exact encoder table entry for this sample rate");
    return static_cast<std::uint32_t>(entry-choices.begin());
}
Profile follow_pcm(const Profile& quality,unsigned sample_rate,unsigned bits,bool floating_point) {
    const auto index=quality_index(quality);
    if (floating_point && bits!=32) throw std::invalid_argument("Floating PCM must be 32 bit");
    Profile result{sample_rate,floating_point?24u:bits,bitrates(sample_rate).at(index)};
    quality_index(result);
    return result;
}
std::uint32_t encoded_frame_bytes(const Profile& profile) {
    quality_index(profile);
    // Pinned encoder: two channel payloads plus the two-byte frame header.
    // 44.1 kHz still uses 240 samples per block, so its frame is longer than 5 ms.
    const auto denominator=profile.sample_rate==44100?147000u:160000u;
    return 2+2*(profile.kbps*1000u*50u/denominator);
}
}
