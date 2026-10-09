#include "audio/pcm_queue.hpp"
#include "audio/wav.hpp"
#include <array>
#include <iostream>
#include <algorithm>
#include <stdexcept>
static void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> static void rejects(F&& action) {
    try { action(); } catch (const std::exception&) { return; }
    throw std::runtime_error("Expected audio operation rejection");
}
int main() {
    try {
        rejects([]{lhdc::PcmQueue bad(17,4);});
        lhdc::PcmQueue queue(32,4);
        std::array<std::uint8_t,40> input{};
        for (unsigned i=0;i<input.size();++i) input[i]=static_cast<std::uint8_t>(i);
        queue.push(std::span(input).first(24));
        std::array<std::uint8_t,16> first{};
        require(queue.pop(first) && std::equal(first.begin(),first.end(),input.begin()),"PCM queue initial order");
        queue.push(std::span(input).subspan(24));
        std::array<std::uint8_t,24> wrapped{};
        require(queue.pop(wrapped) && std::equal(wrapped.begin(),wrapped.end(),input.begin()+16),"PCM queue preserves sample order through wrap and packet boundaries");
        const auto stats=queue.stats();
        require(stats.pushed==40 && stats.popped==40 && stats.high_water==24 && !stats.queued,"PCM queue byte accounting");
        require(!queue.pop(first),"Empty queue never produces invented PCM");
        rejects([&]{queue.push(std::span(input).first(3));});
        rejects([&]{queue.pop(std::span(first).first(3));});
        lhdc::PcmQueue full(16,4);
        full.push(std::span(input).first(8));
        rejects([&]{full.push(std::span(input).first(12));});
        require(full.stats().overruns==1 && full.stats().queued==8,"Overrun does not overwrite buffered audio");
        std::array<std::uint8_t,8> old{};
        require(full.pop(old) && std::equal(old.begin(),old.end(),input.begin()),"Buffered audio survives rejected overrun");
        const auto path=std::filesystem::current_path()/"audio-roundtrip.wav";
        for (const auto bits:{16u,24u}) {
            lhdc::Wav wav{44100,bits,{}};
            for (const auto value:{0,1,-1,32767,-32768}) {
                for (unsigned channel=0;channel<2;++channel) {
                    const auto sample=static_cast<std::uint32_t>(channel?value:-value);
                    for (unsigned b=0;b<bits/8;++b) wav.pcm.push_back(static_cast<std::uint8_t>(sample>>(8*b)));
                }
            }
            lhdc::write_wav(path,wav);
            const auto read=lhdc::read_wav(path);
            require(read.sample_rate==wav.sample_rate && read.bits==wav.bits && read.pcm==wav.pcm,"Captured PCM WAV preserves channel bytes and signed samples");
        }
        for (const auto bits:{16u,24u}) {
            lhdc::write_test_wav(path,1,96000,bits,false,true);
            const auto silent=lhdc::read_wav(path);
            require(silent.sample_rate==96000 && silent.bits==bits && silent.pcm.size()==96000*2*(bits/8),"Silent test WAV has the exact selected format and duration");
            require(std::all_of(silent.pcm.begin(),silent.pcm.end(),[](auto value){return value==0;}),"Silent verification must not emit a test tone");
        }
        std::filesystem::remove(path);
        std::cout << "Audio queue bounds, sample order and packed PCM round trips passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
