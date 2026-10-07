#include "wav.hpp"
#include "codec/profile.hpp"
#include <fstream>
#include <stdexcept>
#include <cmath>
#include <numbers>
#include <array>
#include <algorithm>
#include <string_view>
namespace lhdc {
static std::uint32_t le(const std::uint8_t* p, unsigned count) {
    std::uint32_t value=0;
    for (unsigned i=0;i<count;++i) value |= static_cast<std::uint32_t>(p[i])<<(8*i);
    return value;
}
static void put(std::ostream& out,std::uint32_t value,unsigned count) {
    for (unsigned i=0;i<count;++i) out.put(static_cast<char>((value>>(8*i))&255));
}
Wav read_wav(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open WAV");
    in.exceptions(std::ios::badbit | std::ios::failbit);
    const auto file_size=std::filesystem::file_size(path);
    std::array<std::uint8_t,12> header;
    in.read(reinterpret_cast<char*>(header.data()),header.size());
    if (std::string_view(reinterpret_cast<char*>(header.data()),4)!="RIFF" ||
        std::string_view(reinterpret_cast<char*>(header.data()+8),4)!="WAVE") throw std::runtime_error("Expected RIFF/WAVE");
    const std::uint64_t riff_end=8ull+le(header.data()+4,4);
    if (riff_end>file_size || riff_end<12) throw std::runtime_error("Invalid RIFF length");
    Wav result{};
    bool fmt=false, data=false;
    std::uint64_t offset=12;
    while (offset<riff_end) {
        if (riff_end-offset<8) throw std::runtime_error("Truncated WAV chunk header");
        std::array<std::uint8_t,8> chunk;
        in.read(reinterpret_cast<char*>(chunk.data()),chunk.size());
        const auto size=le(chunk.data()+4,4);
        const std::uint64_t padded=static_cast<std::uint64_t>(size)+(size&1);
        if (padded>riff_end-offset-8) throw std::runtime_error("WAV chunk exceeds RIFF boundary");
        const std::string_view id(reinterpret_cast<char*>(chunk.data()),4);
        if (id=="fmt ") {
            if (fmt || size<16) throw std::runtime_error("Invalid WAV format chunk");
            std::array<std::uint8_t,16> format;
            in.read(reinterpret_cast<char*>(format.data()),format.size());
            result.sample_rate=le(format.data()+4,4); result.bits=le(format.data()+14,2);
            quality_index(Profile{result.sample_rate,result.bits,400});
            const auto alignment=2u*(result.bits/8);
            if (le(format.data(),2)!=1 || le(format.data()+2,2)!=2 ||
                le(format.data()+12,2)!=alignment || le(format.data()+8,4)!=result.sample_rate*alignment)
                throw std::runtime_error("Expected packed PCM S16LE/S24LE stereo WAV with consistent format fields");
            in.seekg(size-16,std::ios::cur);
            fmt=true;
        } else if (id=="data") {
            if (data) throw std::runtime_error("Duplicate WAV data chunk");
            result.pcm.resize(size);
            if (size) in.read(reinterpret_cast<char*>(result.pcm.data()),size);
            data=true;
        } else {
            in.seekg(size,std::ios::cur);
        }
        if (size&1) in.seekg(1,std::ios::cur);
        offset+=8+padded;
    }
    if (!fmt || !data || result.pcm.empty()) throw std::runtime_error("WAV is missing PCM data or format");
    if (result.pcm.size()%(2u*(result.bits/8))) throw std::runtime_error("WAV data contains a partial stereo sample");
    return result;
}
static void write_header(std::ostream& out,std::uint32_t rate,std::uint32_t bits,std::uint32_t bytes) {
    const auto alignment=2u*(bits/8);
    out.write("RIFF",4); put(out,36+bytes,4); out.write("WAVEfmt ",8);
    put(out,16,4); put(out,1,2); put(out,2,2); put(out,rate,4); put(out,rate*alignment,4); put(out,alignment,2); put(out,bits,2);
    out.write("data",4); put(out,bytes,4);
}
void write_wav(const std::filesystem::path& path,const Wav& wav) {
    quality_index(Profile{wav.sample_rate,wav.bits,400});
    if (wav.pcm.empty() || wav.pcm.size()%(2u*(wav.bits/8)) || wav.pcm.size()>UINT32_MAX-36u) throw std::invalid_argument("Invalid PCM WAV output size");
    std::ofstream out(path,std::ios::binary);
    if (!out) throw std::runtime_error("Cannot create WAV");
    out.exceptions(std::ios::badbit | std::ios::failbit);
    write_header(out,wav.sample_rate,wav.bits,static_cast<std::uint32_t>(wav.pcm.size()));
    out.write(reinterpret_cast<const char*>(wav.pcm.data()),wav.pcm.size());
    out.close();
}
void write_test_wav(const std::filesystem::path& path,unsigned seconds,std::uint32_t rate,std::uint32_t bits,bool loop) {
    quality_index(Profile{rate,bits,400});
    if (seconds==0 || seconds>600) throw std::invalid_argument("Test tone duration must be 1..600 seconds");
    const auto frames=seconds*rate;
    const auto alignment=2u*(bits/8);
    std::ofstream out(path,std::ios::binary);
    if (!out) throw std::runtime_error("Cannot create WAV");
    out.exceptions(std::ios::badbit | std::ios::failbit);
    write_header(out,rate,bits,frames*alignment);
    for (std::uint32_t i=0;i<frames;++i) {
        const double time=static_cast<double>(i)/rate;
        // Whole-second 440/880 Hz tones have an integral cycle count. Omit
        // endpoint fades for loops so the source does not create periodic dips.
        const double envelope=loop?1.0:std::min({1.0,time/0.01,static_cast<double>(frames-1-i)/rate/0.01});
        for (double frequency : {440.0,880.0}) {
            const auto sample=static_cast<std::int32_t>(std::lround(1036.0*(bits==16?1:256)*envelope*std::sin(2*std::numbers::pi*frequency*time)));
            put(out,static_cast<std::uint32_t>(sample),bits/8);
        }
    }
}
}
