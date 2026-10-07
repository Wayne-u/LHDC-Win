#include "audio/pcm_converter.hpp"
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
int main() {
    try {
        bool rejected=false;
        try { lhdc::PcmConverter wrong_rate(44100,24,false,{48000,24,400}); }
        catch (const std::invalid_argument&) { rejected=true; }
        if (!rejected) throw std::runtime_error("Cross-rate PCM must be rejected");
        constexpr double pi=3.14159265358979323846;
        unsigned checked=0;
        for (auto rate:{44100u,48000u,96000u,192000u}) for (auto bits:{16u,24u}) {
            lhdc::PcmConverter converter(rate,bits,false,{rate,bits,400});
            std::vector<std::uint8_t> input(960*(bits/8));
            for (std::size_t i=0;i<input.size();++i) input[i]=static_cast<std::uint8_t>(i*37);
            if (converter.convert(input)!=input) throw std::runtime_error("Native-rate integer PCM must remain bit-exact");
        }
        for (auto rate:{44100u,48000u,96000u,192000u}) {
            lhdc::PcmConverter converter(rate,32,true,{rate,24,400});
            const float samples[]={-2.0f,-1.0f,-0.5f,0.0f,0.5f,1.0f,2.0f,0.0f};
            const std::int32_t expected[]={-8388608,-8388608,-4194304,0,4194304,8388607,8388607,0};
            const auto input=std::span(reinterpret_cast<const std::uint8_t*>(samples),sizeof(samples));
            const auto output=converter.convert(input);
            if (output.size()!=24) throw std::runtime_error("Same-rate float conversion changed the sample count");
            for (unsigned i=0;i<8;++i) {
                std::int32_t value=output[i*3]|(output[i*3+1]<<8)|(output[i*3+2]<<16);
                if (value&0x800000) value-=0x1000000;
                if (value!=expected[i]) throw std::runtime_error("Float-to-24-bit clipping or scaling changed");
            }
        }
        for (auto input_rate:{44100u,48000u}) for (auto input_bits:{16u,24u,32u})
        for (auto bits:{16u,24u}) {
            const auto rate=input_rate;
            lhdc::PcmConverter converter(input_rate,input_bits,input_bits==32,{rate,bits,rate==44100?240u:256u});
            std::vector<std::uint8_t> output;
            unsigned position=0;
            for (unsigned i=0;i<100;++i) {
                const auto frames=input_rate/100;
                std::vector<std::uint8_t> data(frames*2*(input_bits/8));
                for (unsigned frame=0;frame<frames;++frame) for (unsigned channel=0;channel<2;++channel) {
                    const auto value=0.1*std::sin(2*pi*(channel?880:440)*(position+frame)/input_rate);
                    auto* sample=data.data()+(frame*2+channel)*(input_bits/8);
                    if (input_bits==32) { const float fp=static_cast<float>(value); std::memcpy(sample,&fp,4); }
                    else {
                        const auto pcm=static_cast<std::int32_t>(std::lround(value*(input_bits==16?32768:8388608)));
                        for (unsigned byte=0;byte<input_bits/8;++byte) sample[byte]=static_cast<std::uint8_t>(static_cast<std::uint32_t>(pcm)>>(byte*8));
                    }
                }
                position+=frames;
                auto block=converter.convert(data); output.insert(output.end(),block.begin(),block.end());
            }
            const auto frames=output.size()/(2*(bits/8));
            if (frames!=rate) throw std::runtime_error("Format conversion changed PCM duration");
            for (unsigned channel=0;channel<2;++channel) {
                double energy=0,real=0,imag=0,peak=0;
                const auto frequency=channel?880:440;
                for (std::size_t i=256;i<frames;++i) {
                    const auto* data=output.data()+(i*2+channel)*(bits/8);
                    std::int32_t sample=static_cast<std::int16_t>(data[0]|(data[1]<<8));
                    if (bits==24) { sample=data[0]|(data[1]<<8)|(data[2]<<16); if (sample&0x800000) sample-=0x1000000; }
                    const double value=sample/(bits==16?32768.0:8388608.0);
                    const double angle=2*pi*frequency*i/rate;
                    real+=value*std::cos(angle); imag+=value*std::sin(angle);
                    energy+=value*value; peak=std::max(peak,std::abs(value));
                }
                const auto count=frames-256;
                const auto amplitude=2*std::hypot(real,imag)/count;
                if (peak<0.099 || peak>0.102 || amplitude<0.098 || amplitude>0.102 || energy/count<0.0048)
                    throw std::runtime_error("Format conversion changed channel frequency or level");
            }
            ++checked;
        }
        std::cout << checked << " PCM representation conversions preserve duration, stereo frequency and level\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
