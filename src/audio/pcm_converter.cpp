#include "pcm_converter.hpp"
#include <algorithm>
#include <cstring>
#include <cmath>
#include <stdexcept>
namespace lhdc {
PcmConverter::PcmConverter(unsigned input_rate,unsigned input_bits,bool floating_point,Profile output)
    :input_align_(2*(input_bits/8)),input_bits_(input_bits),output_bits_(output.bits),floating_point_(floating_point) {
    if (input_rate!=output.sample_rate) throw std::invalid_argument("LHDC sample rate must follow WaveRT input");
    if ((floating_point && input_bits!=32) || (!floating_point && input_bits!=16 && input_bits!=24) || (output.bits!=16 && output.bits!=24))
        throw std::invalid_argument("Unsupported PCM representation");
}
std::vector<std::uint8_t> PcmConverter::convert(std::span<const std::uint8_t> input) {
    if (input.empty() || input.size()%input_align_) throw std::invalid_argument("PCM input must contain complete frames");
    if (!floating_point_ && input_bits_==output_bits_) return {input.begin(),input.end()};
    const auto samples=input.size()/(input_bits_/8);
    std::vector<std::uint8_t> result(samples*(output_bits_/8));
    const auto scale=1<<(output_bits_-1);
    for (std::size_t i=0;i<samples;++i) {
        const auto* source=input.data()+i*(input_bits_/8);
        std::int32_t value=0;
        if (floating_point_) {
            float sample_value; std::memcpy(&sample_value,source,4);
            if (!std::isfinite(sample_value)) throw std::invalid_argument("Non-finite PCM sample");
            value=static_cast<std::int32_t>(std::lround(std::clamp(static_cast<double>(sample_value),-1.0,1.0)*scale));
        } else {
            value=static_cast<std::int16_t>(source[0]|(source[1]<<8));
            if (input_bits_==24) { value=source[0]|(source[1]<<8)|(source[2]<<16); if (value&0x800000) value-=0x1000000; }
            if (output_bits_>input_bits_) value*=256;
            else value=(value+128)>>8;
        }
        value=std::clamp(value,-scale,scale-1);
        for (unsigned byte=0;byte<output_bits_/8;++byte) result[i*(output_bits_/8)+byte]=static_cast<std::uint8_t>(static_cast<std::uint32_t>(value)>>(byte*8));
    }
    return result;
}
}
