#include "avdtp/protocol.hpp"
#include "avdtp/lhdc_v5.hpp"
#include "codec/encoder.hpp"
#include "audio/wav.hpp"
#include "media/packetizer.hpp"
#include "json.hpp"
#ifdef _WIN32
#include "transport/windows.hpp"
#include "audio/wasapi.hpp"
#include "audio/direct_pcm.hpp"
#include "audio/hfp_activity.hpp"
#include "service/status.hpp"
#include "control/rfcomm.hpp"
#endif
#include <iostream>
#include <fstream>
#include <chrono>
#include <algorithm>
#include <stdexcept>
#include <cmath>
namespace av=lhdc::avdtp;
static void write_u32(std::ostream& out,std::uint32_t value) {
    for (unsigned i=0;i<4;++i) out.put(static_cast<char>((value>>(i*8))&255));
}
static std::filesystem::path file_path(const char* text) {
    const std::string value(text);
    return std::filesystem::path(std::u8string(value.begin(),value.end()));
}
static void encode_wav(const char* input,const char* output,std::uint32_t mtu,std::uint32_t kbps=400,bool media=false) {
    const auto wav=lhdc::read_wav(file_path(input));
    if (media && mtu<=lhdc::media_header_bytes) throw std::invalid_argument("Media MTU is smaller than headers");
    lhdc::Encoder encoder(lhdc::Profile{wav.sample_rate,wav.bits,kbps},media?mtu-lhdc::media_header_bytes:mtu);
    lhdc::Packetizer packetizer(media?mtu:mtu+lhdc::media_header_bytes,encoder.block_samples());
    std::ofstream out(file_path(output),std::ios::binary);
    if (!out) throw std::runtime_error("Cannot create record file");
    out.exceptions(std::ios::badbit | std::ios::failbit);
    std::vector<double> times;
    std::uint64_t emitted=0,packets=0,payload_bytes=0;
    std::uint32_t max_packet=0;
    const auto blocks=(wav.pcm.size()+encoder.block_bytes()-1)/encoder.block_bytes();
    std::vector<std::uint8_t> tail(encoder.block_bytes());
    auto emit=[&](std::span<const std::uint8_t> pcm) {
        const auto start=std::chrono::steady_clock::now();
        const auto packet=encoder.encode(pcm);
        const auto end=std::chrono::steady_clock::now();
        times.push_back(std::chrono::duration<double,std::micro>(end-start).count());
        const auto bytes=media?packetizer.pack(packet):packet.payload;
        write_u32(out,static_cast<std::uint32_t>(bytes.size()));
        write_u32(out,packet.frames);
        if (!packet.payload.empty()) {
            out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
            ++packets; emitted+=packet.frames; payload_bytes+=packet.payload.size();
            max_packet=std::max(max_packet,static_cast<std::uint32_t>(bytes.size()));
        }
    };
    for (std::size_t offset=0;offset<wav.pcm.size();offset+=encoder.block_bytes()) {
        auto pcm=std::span(wav.pcm).subspan(offset,std::min(encoder.block_bytes(),wav.pcm.size()-offset));
        if (pcm.size()!=encoder.block_bytes()) {
            std::copy(pcm.begin(),pcm.end(),tail.begin()); pcm=tail;
        }
        emit(pcm);
    }
    if (media) {
        // The upstream API has no partial-packet flush. Add explicit silence until
        // the final real block leaves the frame queue, bounded by the wire count.
        std::fill(tail.begin(),tail.end(),std::uint8_t{0});
        while (emitted<blocks) {
            if (times.size()-blocks>=63) throw std::runtime_error("Encoder tail did not drain within LHDC packet frame limit");
            emit(tail);
        }
    }
    out.close();
    std::sort(times.begin(),times.end());
    auto percentile=[&](double fraction){return times[static_cast<std::size_t>(std::ceil(fraction*times.size()))-1];};
    const auto pending=times.size()-emitted;
    std::cout << "{\"event\":" << lhdc::json_string(media?"offline_packetize":"offline_encode") << ",\"sample_rate\":" << wav.sample_rate
        << ",\"bits\":" << wav.bits << ",\"channels\":2,\"target_kbps\":" << kbps << ",\"mtu\":" << mtu
        << ",\"payload_mtu\":" << (media?mtu-lhdc::media_header_bytes:mtu) << ",\"media_headers\":" << (media?14:0)
        << ",\"padded_samples\":" << (blocks*encoder.block_samples()-wav.pcm.size()/(2u*(wav.bits/8)))
        << ",\"block_samples\":" << encoder.block_samples() << ",\"input_blocks\":" << blocks
        << ",\"encode_calls\":" << times.size() << ",\"silent_padding_blocks\":" << (times.size()-blocks)
        << ",\"emitted_frames\":" << emitted << ",\"pending_frames_at_eof\":" << pending
        << ",\"nonempty_packets\":" << packets << ",\"payload_bytes\":" << payload_bytes << ",\"max_packet\":" << max_packet
        << ",\"encode_us\":{\"p50\":" << percentile(.50) << ",\"p95\":" << percentile(.95) << ",\"p99\":" << percentile(.99)
        << "},\"bluetooth_sent\":false}\n";
}
static void print_caps(const av::Bytes& raw,unsigned seid) {
    std::ostringstream out;
    out << "{\"event\":\"capabilities\",\"seid\":" << seid << ",\"raw\":" << lhdc::json_string(av::hex(raw)) << ",\"services\":[";
    bool first=true;
    for (const auto& cap : av::capabilities(raw)) {
        if (!first) out << ',';
        first=false;
        out << "{\"category\":" << unsigned(cap.category) << ",\"data\":" << lhdc::json_string(av::hex(cap.data));
        if (const auto vendor=av::vendor_codec(cap))
            out << ",\"vendor_id\":" << vendor->vendor_id << ",\"codec_id\":" << vendor->codec_id
                << ",\"lhdc_v5\":" << (vendor->vendor_id==av::lhdc_v5_vendor && vendor->codec_id==av::lhdc_v5_codec ? "true":"false");
        out << '}';
    }
    out << "]}\n";
    std::cout << out.str();
}
#ifdef _WIN32
static std::uint64_t address(const std::string& text) {
    std::string clean;
    for (char c:text) if (c!=':') clean+=c;
    if (clean.size()!=12 || av::unhex(clean).size()!=6) throw std::invalid_argument("Bluetooth address must contain 12 hex digits");
    return std::stoull(clean,nullptr,16);
}
#endif
static std::uint32_t number(const char* text) {
    std::size_t used=0;
    const auto value=std::stoul(text,&used);
    if (used!=std::string(text).size()) throw std::invalid_argument("Expected an unsigned integer");
    return static_cast<std::uint32_t>(value);
}
static void profile_options(const av::Bytes& raw,std::uint32_t mtu=0) {
    if(mtu && mtu<=lhdc::media_header_bytes) throw std::invalid_argument("Media MTU is smaller than packet headers");
    const auto caps=av::v5_capabilities(raw);
    std::cout << "{\"event\":\"profile_options\",\"min_kbps\":" << caps.min_kbps << ",\"max_kbps\":" << caps.max_kbps << ",\"bits\":[";
    for (std::size_t i=0;i<caps.bit_depths.size();++i) { if(i) std::cout<<','; std::cout<<caps.bit_depths[i]; }
    std::cout << "],\"rates\":[";
    for (std::size_t i=0;i<caps.sample_rates.size();++i) {
        if (i) std::cout<<',';
        const auto rate=caps.sample_rates[i];
        std::cout << "{\"sample_rate\":" << rate << ",\"bitrates\":[";
        bool first=true;
        for (const auto kbps:lhdc::bitrates(rate)) {
            if (kbps<caps.min_kbps || kbps>caps.max_kbps) continue;
            if(mtu && lhdc::encoded_frame_bytes({rate,16,kbps})>mtu-lhdc::media_header_bytes) continue;
            if (!first) std::cout<<','; first=false; std::cout<<kbps;
        }
        std::cout << "]}";
    }
    std::cout << "]}\n";
}
static int run(int argc,char** argv) {
    std::cout << std::unitbuf;
    try {
        if (argc<2) throw std::invalid_argument("Usage: lhdc-host target | hires [on|off] | inspect | connection ADDRESS | audio-status | hfp-status | call-probe SECONDS | audio-devices | audio-inputs | direct-pcm | direct-formats | direct-capture SECONDS FILE | render-wav ENDPOINT WAV [REPEATS] | make-test-wav FILE [SECONDS [RATE BITS]] | encode WAV RECORDS [PAYLOAD_MTU [KBPS]] | packetize WAV RECORDS OUT_MTU KBPS | profile-check RATE BITS KBPS | profile-options HEX | configure HEX RATE BITS KBPS | decode-caps HEX");
        const std::string command=argv[1];
        if (command=="make-test-wav" && (argc==3 || argc==4 || argc==6 || argc==7)) {
            if (argc==7 && std::string(argv[6])!="--loop") throw std::invalid_argument("Expected --loop after the tone format");
            lhdc::write_test_wav(file_path(argv[2]),argc>=4?number(argv[3]):2,argc>=6?number(argv[4]):48000,argc>=6?number(argv[5]):16,argc==7);
            std::cout << "{\"event\":\"wav_created\",\"peak_dbfs\":-30,\"left_hz\":440,\"right_hz\":880}\n";
        } else if (command=="encode" && argc>=4 && argc<=6) encode_wav(argv[2],argv[3],argc>=5?number(argv[4]):660,argc==6?number(argv[5]):400);
        else if (command=="packetize" && argc==6) encode_wav(argv[2],argv[3],number(argv[4]),number(argv[5]),true);
        else if (command=="profile-options" && (argc==3 || argc==4)) profile_options(av::unhex(argv[2]),argc==4?number(argv[3]):0);
        else if (command=="profile-check" && argc==5) {
            const lhdc::Profile profile{number(argv[2]),number(argv[3]),number(argv[4])};
            std::cout << "{\"event\":\"encoder_profile\",\"quality_index\":" << lhdc::quality_index(profile) << "}\n";
        }
        else if (command=="configure" && argc==6) {
            const lhdc::Profile profile{number(argv[3]),number(argv[4]),number(argv[5])};
            const auto selected=av::select_configuration(av::unhex(argv[2]),profile);
            std::cout << "{\"event\":\"profile_validated\",\"configuration\":" << lhdc::json_string(av::hex(selected)) << ",\"negotiated\":false}\n";
        }
        else if (command=="decode-caps" && argc==3) print_caps(av::unhex(argv[2]),0);
#ifdef _WIN32
        else if (command=="inspect" && argc==2) lhdc::inspect_windows();
        else if (command=="target" && argc==2) lhdc::inspect_target();
        else if (command=="hires" && argc==2) lhdc::inspect_hi_res();
        else if (command=="hires" && argc==3 && (std::string(argv[2])=="on" || std::string(argv[2])=="off"))
            lhdc::inspect_hi_res(std::string(argv[2])=="on");
        else if (command=="direct-pcm" && argc==2) lhdc::inspect_direct_pcm();
        else if (command=="direct-formats" && argc==2) lhdc::inspect_direct_formats();
        else if (command=="direct-capture" && argc==4) lhdc::capture_direct_pcm(number(argv[2]),file_path(argv[3]));
        else if (command=="audio-status" && argc==2) lhdc::inspect_audio_service();
        else if (command=="hfp-status" && argc==2) lhdc::inspect_hfp_activity();
        else if (command=="call-probe" && argc==3) lhdc::probe_headphone_microphone(number(argv[2]));
        else if (command=="audio-devices" && argc==2) lhdc::inspect_audio_devices();
        else if (command=="audio-inputs" && argc==2) lhdc::inspect_audio_devices(true);
        else if (command=="playback-ready" && argc==5) lhdc::inspect_playback_ready(argv[2],number(argv[3]),number(argv[4]));
        else if (command=="render-wav" && (argc==4 || argc==5)) lhdc::render_wav(argv[2],file_path(argv[3]),argc==5?number(argv[4]):1);
        else if (command=="connection" && argc==3) lhdc::inspect_connection(address(argv[2]));
#endif
        else throw std::invalid_argument("Unknown command or invalid arguments");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "{\"event\":\"error\",\"message\":" << lhdc::json_string(error.what()) << "}\n";
        return 1;
    }
}
#ifdef _WIN32
int wmain(int argc,wchar_t** argv) {
    std::vector<std::string> arguments;
    for (int i=0;i<argc;++i) {
        const int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,nullptr,0,nullptr,nullptr);
        if (!size) return 1;
        std::string value(static_cast<std::size_t>(size),'\0');
        if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,argv[i],-1,value.data(),size,nullptr,nullptr)) return 1;
        value.pop_back(); arguments.push_back(std::move(value));
    }
    std::vector<char*> pointers;
    for (auto& argument:arguments) pointers.push_back(argument.data());
    return run(argc,pointers.data());
}
#else
int main(int argc,char** argv) { return run(argc,argv); }
#endif
