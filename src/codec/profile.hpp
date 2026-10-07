#pragma once
#include <cstdint>
#include <vector>
namespace lhdc {
struct Profile {
    std::uint32_t sample_rate = 48000;
    std::uint32_t bits = 16;
    std::uint32_t kbps = 400;
};
std::vector<std::uint32_t> bitrates(std::uint32_t sample_rate);
std::uint32_t quality_index(const Profile& profile);
Profile follow_pcm(const Profile& quality,unsigned sample_rate,unsigned bits,bool floating_point);
}
