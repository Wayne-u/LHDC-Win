#pragma once
#include "codec/profile.hpp"
#include <span>
#include <vector>
namespace lhdc {
// Convert PCM representation only; the encoder always follows the WaveRT rate.
class PcmConverter {
public:
    PcmConverter(unsigned input_rate,unsigned input_bits,bool floating_point,Profile output);
    std::vector<std::uint8_t> convert(std::span<const std::uint8_t> input);
private:
    unsigned input_align_,input_bits_,output_bits_;
    bool floating_point_;
};
}
