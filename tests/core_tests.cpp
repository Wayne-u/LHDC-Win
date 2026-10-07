#include "avdtp/protocol.hpp"
#include "avdtp/lhdc_v5.hpp"
#include "codec/encoder.hpp"
#include "audio/wav.hpp"
#include "media/packetizer.hpp"
#include "avdtp/stream.hpp"
#include <iostream>
#include <stdexcept>
#include <filesystem>
#include <fstream>
namespace av=lhdc::avdtp;
static unsigned checks=0;
static void require(bool value,const char* message) {
    ++checks;
    if (!value) throw std::runtime_error(message);
}
template<class F> static void rejects(F function,const char* message) {
    ++checks;
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error(message);
}
int main() {
    try {
        require(av::command(0,av::Signal::discover)==av::Bytes{0,1},"Discover command bytes");
        require(av::command(15,av::Signal::get_capabilities,av::Bytes{4})==av::Bytes{0xf0,2,4},"SEID encoding");
        rejects([]{av::command(16,av::Signal::discover);},"Label range");
        av::Reassembler parser;
        auto response=parser.consume(av::unhex("020104080a08"));
        require(response && response->label==0 && response->type==av::MessageType::accept,"Accept header");
        const auto seps=av::endpoints(response->payload);
        require(seps.size()==2 && seps[0].seid==1 && seps[0].sink && !seps[0].in_use && seps[1].seid==2,"Discover endpoints");
        rejects([]{av::endpoints(av::Bytes{4});},"Odd Discover length");
        rejects([]{av::endpoints(av::Bytes{0,8});},"Zero SEID");
        rejects([]{av::endpoints(av::Bytes{4,8,4,8});},"Duplicate SEID");
        rejects([]{av::endpoints(av::Bytes{0xfc,8});},"Reserved SEID");
        require(!parser.consume(av::unhex("3603020100")),"Start fragment");
        require(!parser.consume(av::unhex("3a070d00ff3a")),"Continue fragment");
        response=parser.consume(av::unhex("3e050000354c10d4110000"));
        require(response && response->label==3 && response->payload==av::local_capabilities(),"Fragment reassembly");
        rejects([&]{parser.consume(av::Bytes{0x0e});},"Orphan end");
        rejects([&]{parser.consume(av::Bytes{0x06,1,2});},"Invalid fragment count");
        parser.consume(av::Bytes{0x06,2,2,1});
        rejects([&]{parser.consume(av::Bytes{0x1e,0});},"Mismatched transaction");
        require(parser.consume(av::Bytes{2,1}).has_value(),"Parser resets after error");
        const auto caps=av::local_capabilities();
        std::vector<av::Signal> steps;
        av::Stream stream([&](av::Signal signal,const av::Bytes& payload) {
            steps.push_back(signal);
            if (signal==av::Signal::get_configuration) return caps;
            if (signal==av::Signal::set_configuration) require(payload==av::unhex("0c040100070d00ff3a050000354c10d4110000"),"Stream source/sink configuration SEIDs");
            else require(payload==av::Bytes{12},"Stream command remote SEID");
            return av::Bytes{};
        },3,caps);
        rejects([&]{stream.start();},"Start before configuration");
        stream.configure(); stream.open();
        rejects([&]{stream.start();},"Start before media L2CAP");
        stream.media_ready(); stream.start(); stream.suspend(); stream.close();
        require(steps==std::vector<av::Signal>{av::Signal::set_configuration,av::Signal::get_configuration,av::Signal::open,av::Signal::start,av::Signal::suspend,av::Signal::close},"AVDTP playback transaction order");
        require(stream.state()==av::StreamState::idle,"Normal close releases configuration");
        steps.clear();
        av::Stream mismatch([&](av::Signal signal,const av::Bytes&) {
            steps.push_back(signal);
            if (signal==av::Signal::get_configuration) return av::Bytes{};
            return av::Bytes{};
        },3,caps);
        rejects([&]{mismatch.configure();},"Configuration readback must match before Open");
        rejects([&]{mismatch.open();},"Readback failure must not permit Open");
        mismatch.abort();
        require(steps==std::vector<av::Signal>{av::Signal::set_configuration,av::Signal::get_configuration,av::Signal::abort},"Accepted config is aborted after readback failure");
        av::Stream rejection([&](av::Signal signal,const av::Bytes&) {
            if (signal==av::Signal::get_configuration) return caps;
            if (signal==av::Signal::start) throw std::runtime_error("Captured Start reject");
            return av::Bytes{};
        },3,caps);
        rejection.configure(); rejection.open(); rejection.media_ready();
        rejects([&]{rejection.start();},"Start reject stops streaming");
        require(rejection.state()==av::StreamState::opened,"Start reject cannot mark stream active");
        rejection.abort();
        const auto tlvs=av::capabilities(caps);
        const auto vendor=av::vendor_codec(tlvs[1]);
        require(tlvs.size()==2 && vendor && vendor->vendor_id==0x53a && vendor->codec_id==0x4c35 && vendor->data.size()==5,"Vendor codec endianness");
        require(av::select_48k_s16_400(caps)==caps,"LHDC intersection");
        // Enco X4 capture: 2026-10-04, transport-20261004-214048-850-Test.
        av::Reassembler captured;
        const auto discovered=captured.consume(av::unhex("0201040808080c08"));
        require(discovered.has_value(),"Captured Discover response");
        const auto actual_seps=av::endpoints(discovered->payload);
        require(actual_seps.size()==3 && actual_seps[2].seid==3 && actual_seps[2].sink && !actual_seps[2].in_use,"Captured LHDC sink SEP");
        const auto actual_caps=captured.consume(av::unhex("32020100070d00ff3a050000354c3016114000"));
        require(actual_caps.has_value(),"Captured GetCapabilities response");
        require(av::select_48k_s16_400(actual_caps->payload)==caps,"Captured peer accepts fixed format intersection");
        const auto peer=av::v5_capabilities(actual_caps->payload);
        require(peer.sample_rates==std::vector<std::uint32_t>{44100,48000} && peer.bit_depths==std::vector<std::uint32_t>{16,24},"Captured rate and depth masks");
        require(peer.min_kbps==64 && peer.max_kbps==400,"Captured bitrate bounds");
        require(av::select_configuration(actual_caps->payload,{44100,24,240})==av::unhex("0100070d00ff3a050000354c2052110000"),"44.1k S24 configuration bytes");
        rejects([&]{av::select_configuration(actual_caps->payload,{96000,16,400});},"Peer does not advertise 96k");
        rejects([&]{av::select_configuration(actual_caps->payload,{48000,24,500});},"Peer bitrate ceiling");
        rejects([]{lhdc::quality_index({44100,16,256});},"44.1k must not silently select 240 for 256");
        const auto followed=lhdc::follow_pcm({48000,24,256},44100,16,false);
        require(followed.sample_rate==44100 && followed.bits==16 && followed.kbps==240,"Follow Windows PCM and preserve the bitrate quality index");
        const auto floating=lhdc::follow_pcm({44100,16,480},192000,32,true);
        require(floating.sample_rate==192000 && floating.bits==24 && floating.kbps==500,"High rate float PCM uses 24-bit encoding without downsampling");
        rejects([]{lhdc::follow_pcm({48000,24,400},88200,24,false);},"Unsupported Windows rate must not silently resample");
        rejects([]{lhdc::follow_pcm({48000,24,400},48000,32,false);},"Unsupported integer PCM representation");
        // Synthetic full-range peer, distinct from the Enco X4 hardware capture.
        const auto full_range=av::unhex("0100070d00ff3a050000354c3506110000");
        require(av::v5_capabilities(full_range).max_kbps==1000,"Peer can advertise the 1 Mbps limit");
        for (const auto rate:{96000u,192000u}) for (const auto bits:{16u,24u})
            for (const auto kbps:{500u,900u,1000u}) {
                av::select_configuration(full_range,{rate,bits,kbps});
                lhdc::Encoder encoder({rate,bits,kbps},672-lhdc::media_header_bytes);
                lhdc::Packetizer media(672,encoder.block_samples());
                av::Bytes samples(encoder.block_bytes());
                std::uint32_t random=1; unsigned emitted=0;
                for (unsigned block=0;block<24;++block) {
                    for (auto& byte:samples) { random=random*1664525u+1013904223u; byte=static_cast<std::uint8_t>(random>>24); }
                    const auto packet=encoder.encode(samples);
                    require(media.pack(packet).size()<=672,"High rate and bitrate profiles respect actual media packet budget");
                    emitted+=packet.frames;
                }
                require(emitted>0,"High rate encoder emits media frames");
            }
        for (const auto rate:peer.sample_rates) for (const auto bits:peer.bit_depths)
            for (const auto kbps:lhdc::bitrates(rate)) {
                if (kbps>peer.max_kbps) continue;
                av::select_configuration(actual_caps->payload,{rate,bits,kbps});
                lhdc::Encoder encoder({rate,bits,kbps},658);
                lhdc::Packetizer media(672,encoder.block_samples());
                std::uint32_t encoded=0;
                for (unsigned block=0;block<24;++block) {
                    const auto packet=encoder.encode(av::Bytes(encoder.block_bytes()));
                    const auto sdu=media.pack(packet);
                    require(sdu.size()<=672,"All advertised profiles respect media MTU");
                    if (!sdu.empty()) {
                        require(sdu[0]==0x80 && sdu[1]==0x60 && sdu[12]==packet.frames*4,"Media header and frame count");
                        encoded+=packet.frames;
                    }
                }
                require(encoded>0 && encoded<=24,"Profile produces actual frames");
            }
        lhdc::Packetizer wrapping(20,240,65535,0xffffff00u,255);
        require(wrapping.pack({{},0}).empty(),"Buffered empty encode has no RTP packet");
        require(wrapping.pack({{0xaa,0xbb},2})==av::unhex("8060ffffffff ff000000000108ffaabb"),"Exact RTP/LHDC bytes before rollover");
        require(wrapping.pack({{0xcc},1})==av::unhex("80600000000000e0000000010400cc"),"RTP timestamp and both sequence rollovers");
        rejects([&]{wrapping.pack({av::Bytes(7),1});},"Packetizer MTU boundary");
        rejects([&]{wrapping.pack({{1},64});},"LHDC count overflow");
        const auto configured=captured.consume(av::unhex("4203"));
        require(configured && configured->type==av::MessageType::accept && configured->signal==av::Signal::set_configuration && configured->payload.empty(),"Captured SetConfiguration Accept");
        const auto readback=captured.consume(av::unhex("52040100070d00ff3a050000354c10d4110000"));
        require(readback && readback->payload==caps,"Captured configuration readback");
        auto incompatible=caps; incompatible[12]=0x04;
        rejects([&]{av::select_48k_s16_400(incompatible);},"Peer missing 48k");
        incompatible=caps; incompatible[13]=0xd2;
        rejects([&]{av::select_48k_s16_400(incompatible);},"Peer missing S16");
        incompatible=caps; incompatible[14]=0x10;
        rejects([&]{av::select_48k_s16_400(incompatible);},"Peer missing version1");
        incompatible=caps; incompatible.insert(incompatible.end(),{4,2,2,0});
        rejects([&]{av::select_48k_s16_400(incompatible);},"Unsupported protection");
        rejects([]{av::capabilities(av::Bytes{7,13,0});},"Truncated TLV");
        rejects([]{av::vendor_codec(av::Capability{7,{0,255}});},"Truncated vendor ID");
        rejects([]{av::unhex("012");},"Odd hex input");
        rejects([]{av::unhex("0z");},"Invalid hex input");
        require(av::unhex("00 01\nff")==av::Bytes{0,1,255},"Whitespace in hex");
        av::verify_configuration(caps,caps);
        rejects([&]{auto actual=caps;actual[12]=4;av::verify_configuration(caps,actual);},"GetConfiguration mismatch");
        lhdc::Encoder first,second;
        require(first.block_samples()==240 && first.block_bytes()==960,"PCM block dimensions");
        rejects([&]{first.encode(av::Bytes(959));},"Partial PCM input");
        rejects([]{lhdc::Encoder bad(299);},"Invalid payload MTU");
        av::Bytes pcm(first.block_bytes());
        std::uint32_t state=0x12345678;
        unsigned frames=0,packets=0,empty=0;
        for (unsigned i=0;i<100;++i) {
            for (auto& byte:pcm) {state=state*1664525u+1013904223u;byte=static_cast<std::uint8_t>(state>>24);}
            const auto a=first.encode(pcm),b=second.encode(pcm);
            require(a.payload==b.payload && a.frames==b.frames,"Independent encoder determinism");
            require(a.payload.size()<=660,"Encoder payload MTU");
            if (a.payload.empty()) {++empty;require(a.frames==0,"Empty output is not a frame");}
            else {++packets;frames+=a.frames;}
        }
        require(frames==100 && packets==50 && empty==50,"Buffered encoder output boundaries");
        const auto path=std::filesystem::current_path()/"core-test-tone.wav";
        lhdc::write_test_wav(path,1);
        const auto wav=lhdc::read_wav(path);
        require(wav.sample_rate==48000 && wav.pcm.size()==192000,"WAV round trip");
        {
            std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);
            file.seekp(22);file.put(1); // mono declaration contradicts stereo PCM
        }
        rejects([&]{lhdc::read_wav(path);},"Unsupported PCM format");
        std::filesystem::remove(path);
        for (const auto rate:{44100u,48000u}) {
            lhdc::write_test_wav(path,1,rate,24);
            const auto packed=lhdc::read_wav(path);
            require(packed.sample_rate==rate && packed.bits==24 && packed.pcm.size()==rate*6,"Packed S24 WAV round trip");
            std::filesystem::remove(path);
        }
        std::cout << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
