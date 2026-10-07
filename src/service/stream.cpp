#include "stream.hpp"
#include "progress_log.hpp"
#include <sstream>
#include <syncstream>
#include "audio/direct_pcm.hpp"
#include "audio/hfp_activity.hpp"
#include "audio/pcm_reader.hpp"
#include "audio/windows_audio.hpp"
#include "host/media_session.hpp"
#include "host/pacer.hpp"
#include "host/json.hpp"
#include "media/packetizer.hpp"
#include "transport/windows.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <algorithm>
namespace lhdc {
Profile service_profile() {
    Profile profile{}; DWORD size=sizeof(profile);
    static_assert(sizeof(Profile)==12);
    const auto result=RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\LHDC-Win",L"Profile",RRF_RT_REG_BINARY,nullptr,&profile,&size);
    if (result!=ERROR_SUCCESS || size!=sizeof(profile)) throw std::runtime_error("Service codec profile is missing or invalid, Win32="+std::to_string(result));
    quality_index(profile);
    return profile;
}
namespace {
bool same(Profile a,Profile b) { return a.sample_rate==b.sample_rate && a.bits==b.bits && a.kbps==b.kbps; }
bool stopped(HANDLE stop) {
    const auto result=WaitForSingleObject(stop,0);
    if (result!=WAIT_OBJECT_0 && result!=WAIT_TIMEOUT) throw std::runtime_error("Service stop wait failed");
    return result==WAIT_OBJECT_0;
}
void transfer(DirectPcm& pcm,HANDLE stop,Profile profile,Profile& last_format,const HfpActivity& hfp) {
    AudioTask priority; Pacer pacer;
    const auto quality=profile;
    MediaSession session(target_device().address,profile); session.discover();
    const auto& caps=session.peer_capabilities();
    const auto saved=RegSetKeyValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\LHDC-Win",L"PeerCapabilities",REG_BINARY,caps.data(),static_cast<DWORD>(caps.size()));
    if (saved!=ERROR_SUCCESS) throw std::runtime_error("Peer capability cache write failed, Win32="+std::to_string(saved));
    LHDC_PCM_STATE state{};
    // Format probes can overwrite idle PCM metadata. Prepare using the last
    // running format, then verify against the actual stream before sending.
    profile=follow_pcm(quality,last_format.sample_rate,last_format.bits,false);
    session.configure(profile);
    auto next_hfp_check=std::chrono::steady_clock::now();
    while (!stopped(stop)) {
        session.check();
        if (!same(quality,service_profile())) return;
        state=pcm.state();
        if (state.Running) {
            last_format=follow_pcm(quality,state.SampleRate,state.Bits,state.FloatingPoint!=0);
            if (!same(profile,last_format)) return;
            break;
        }
        if(std::chrono::steady_clock::now()>=next_hfp_check) {
            if(hfp.active()) return;
            next_hfp_check=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
        }
        if (WaitForSingleObject(stop,5)==WAIT_OBJECT_0) return;
    }
    if (stopped(stop)) return;
    session.check();
    if (state.Channels!=2 || state.BlockAlign!=2*(state.Bits/8)) throw std::runtime_error("Direct PCM requires interleaved stereo");
    Encoder encoder(profile,session.media_info().OutMtu-media_header_bytes);
    Packetizer packetizer(session.media_info().OutMtu,encoder.block_samples());
    const std::size_t byte_rate=static_cast<std::size_t>(profile.sample_rate)*2*(profile.bits/8);
    const auto preroll_bytes=byte_rate*60/1000;
    // Start can wait for the peer while Windows continues producing PCM.
    // Complete that exchange before discarding stale startup data and preroll.
    session.start(encoder.block_samples());
    // Preserve the beginning of a newly started render stream. Discard only
    // stale backlog accumulated while negotiating or restarting the encoder.
    const auto pending=pcm.state();
    if (pending.DroppedBytes || pending.BufferedBytes>static_cast<std::uint64_t>(pending.SampleRate)*pending.BlockAlign/10) pcm.reset();
    PcmReader reader([&](std::size_t count){return pcm.read(count);},[&]{pcm.reset();},state,profile);
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    auto started=std::chrono::steady_clock::now();
    std::uint64_t frames=0,packets=0,bytes=0,nonzero_blocks=0;
    std::uint64_t overrun_recoveries=0;
    std::uint64_t send_stalls=0;
    std::int64_t max_hfp_check_us=0;
    std::int64_t max_send_us=0,max_submit_us=0,max_io_wait_us=0,max_completion_us=0,max_encode_us=0,max_wait_us=0,max_report_us=0,max_profile_us=0,max_state_us=0,max_log_us=0;
    auto elapsed_us=[](auto start) { return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count(); };
    while (!stopped(stop)) {
        const auto input=reader.stats();
        if(input.epoch_changed) return;
        if(input.queue.queued>=preroll_bytes) break;
        if(input.finished) return;
        if (std::chrono::steady_clock::now()>deadline) throw std::runtime_error("Direct PCM preroll timeout");
        if (WaitForSingleObject(stop,2)==WAIT_OBJECT_0) return;
    }
    if (stopped(stop)) return;
    // Catch up extra startup/recovery backlog while retaining the 60ms lead.
    auto pace_origin=[&](const PcmReadStats& input) {
        const auto extra=input.queue.queued>preroll_bytes ? input.queue.queued-preroll_bytes : 0;
        const auto queued_frames=extra/(2*(profile.bits/8));
        return std::chrono::steady_clock::now()-std::chrono::nanoseconds((frames*encoder.block_samples()+queued_frames)*1000000000ull/profile.sample_rate);
    };
    started=pace_origin(reader.stats()); auto report=std::chrono::steady_clock::now();
    std::vector<std::uint8_t> block(encoder.block_bytes());
    std::osyncstream(std::cout) << "{\"event\":\"direct_audio_started\",\"source\":\"WaveRT\",\"sample_rate\":" << profile.sample_rate << ",\"bits\":" << profile.bits << ",\"kbps\":" << profile.kbps
        << ",\"input_sample_rate\":" << state.SampleRate << ",\"input_bits\":" << state.Bits << ",\"input_float\":" << (state.FloatingPoint?"true":"false") << ",\"resampling\":false}" << std::endl;
    ProgressLog progress(std::cout);
    while (!stopped(stop)) {
        if(std::chrono::steady_clock::now()>=next_hfp_check) {
            const auto check_started=std::chrono::steady_clock::now();
            const bool calling=hfp.active();
            max_hfp_check_us=std::max(max_hfp_check_us,elapsed_us(check_started));
            if(calling) break;
            next_hfp_check=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
        }
        session.check();
        const auto input=reader.stats();
        if(input.epoch_changed) break;
        if(input.overrun_recoveries!=overrun_recoveries) {
            overrun_recoveries=input.overrun_recoveries;
            started=pace_origin(input);
        }
        if (!reader.pop(block)) {
            const auto empty=reader.stats();
            if(empty.finished) break;
            if (std::chrono::steady_clock::now()-empty.last_input>std::chrono::milliseconds(250)) throw std::runtime_error("Direct PCM producer stalled");
            if (WaitForSingleObject(stop,2)==WAIT_OBJECT_0) break;
            continue;
        }
        if (std::any_of(block.begin(),block.end(),[](auto value){return value!=0;})) ++nonzero_blocks;
        const auto encode_started=std::chrono::steady_clock::now();
        const auto packet=encoder.encode(block);
        max_encode_us=std::max(max_encode_us,elapsed_us(encode_started));
        const auto sdu=packetizer.pack(packet);
        if (sdu.empty()) continue;
        const auto wait_started=std::chrono::steady_clock::now();
        pacer.wait_until(started+std::chrono::nanoseconds(frames*encoder.block_samples()*1000000000ull/profile.sample_rate));
        max_wait_us=std::max(max_wait_us,elapsed_us(wait_started));
        const auto send_started=std::chrono::steady_clock::now();
        const auto timing=session.send(sdu);
        const auto send_us=elapsed_us(send_started);
        max_send_us=std::max(max_send_us,send_us);
        max_submit_us=std::max(max_submit_us,timing.submit_us);
        max_io_wait_us=std::max(max_io_wait_us,timing.wait_us);
        max_completion_us=std::max(max_completion_us,timing.completion_us);
        constexpr std::int64_t send_stall_warning_us=100000;
        if(send_us>=send_stall_warning_us) {
            ++send_stalls;
            std::osyncstream(std::cout)<<"{\"event\":\"media_send_stall\",\"send_us\":"<<send_us
                <<",\"packet\":"<<(packets+1)<<",\"elapsed_ms\":"<<elapsed_us(started)/1000
                <<",\"submit_us\":"<<timing.submit_us<<",\"io_wait_us\":"<<timing.wait_us<<",\"completion_us\":"<<timing.completion_us
                <<",\"io_pending\":"<<(timing.pending?"true":"false")<<"}"<<std::endl;
        }
        frames+=packet.frames; ++packets; bytes+=sdu.size();
        if (std::chrono::steady_clock::now()-report>=std::chrono::seconds(1)) {
            const auto report_started=std::chrono::steady_clock::now();
            if (!same(quality,service_profile())) break;
            max_profile_us=std::max(max_profile_us,elapsed_us(report_started));
            const auto state_started=std::chrono::steady_clock::now();
            const auto capture=reader.stats();
            const auto& info=capture.state;
            max_state_us=std::max(max_state_us,elapsed_us(state_started));
            const auto log_started=std::chrono::steady_clock::now();
            std::ostringstream snapshot;
            snapshot << "{\"event\":\"direct_audio_progress\",\"packets\":" << packets << ",\"bytes\":" << bytes << ",\"nonzero_blocks\":" << nonzero_blocks
                << ",\"queue_ms\":" << 1000.0*capture.queue.queued/byte_rate << ",\"queue_limit_ms\":"<<pcm_queue_duration_ms<<",\"queue_high_water_bytes\":"<<capture.queue.high_water
                << ",\"buffered_bytes\":" << info.BufferedBytes << ",\"produced_bytes\":" << info.ProducedBytes
                << ",\"dropped_bytes\":" << info.DroppedBytes << ",\"dropped_bytes_total\":" << capture.dropped_bytes() << ",\"overrun_recoveries\":" << capture.overrun_recoveries
                << ",\"kernel_dropped_bytes_total\":"<<capture.kernel_dropped_bytes<<",\"discarded_pcm_bytes_total\":"<<capture.discarded_bytes
                << ",\"max_send_us\":" << max_send_us << ",\"max_submit_us\":"<<max_submit_us<<",\"max_io_wait_us\":"<<max_io_wait_us<<",\"max_completion_us\":"<<max_completion_us
                << ",\"send_stalls\":"<<send_stalls<<",\"max_hfp_check_us\":"<<max_hfp_check_us<<",\"max_read_gap_us\":" << capture.max_read_gap_us << ",\"max_read_us\":" << capture.max_read_us
                << ",\"max_queue_wait_us\":"<<capture.max_queue_wait_us
                << ",\"max_encode_us\":" << max_encode_us << ",\"max_wait_us\":" << max_wait_us << ",\"max_report_us\":" << max_report_us << ",\"max_profile_us\":" << max_profile_us << ",\"max_state_us\":" << max_state_us << ",\"max_log_us\":" << max_log_us << "}";
            progress.submit(snapshot.str());
            max_log_us=std::max(max_log_us,elapsed_us(log_started));
            max_report_us=std::max(max_report_us,elapsed_us(report_started));
            report=std::chrono::steady_clock::now();
        }
    }
    reader.stop();
    const auto capture=reader.stats();
    progress.finish();
    const auto finished=session.finish();
    std::osyncstream(std::cout) << "{\"event\":\"direct_audio_complete\",\"packets\":" << packets << ",\"bytes\":" << bytes << ",\"completed_bytes\":" << finished.bytes
        << ",\"nonzero_blocks\":" << nonzero_blocks << ",\"dropped_bytes_total\":" << capture.dropped_bytes() << ",\"overrun_recoveries\":" << capture.overrun_recoveries
        << ",\"kernel_dropped_bytes_total\":"<<capture.kernel_dropped_bytes<<",\"discarded_pcm_bytes_total\":"<<capture.discarded_bytes<<",\"queue_limit_ms\":"<<pcm_queue_duration_ms<<",\"queue_high_water_bytes\":"<<capture.queue.high_water
        << ",\"max_read_gap_us\":"<<capture.max_read_gap_us<<",\"max_queue_wait_us\":"<<capture.max_queue_wait_us
        << ",\"max_send_us\":"<<max_send_us<<",\"max_submit_us\":"<<max_submit_us<<",\"max_io_wait_us\":"<<max_io_wait_us<<",\"max_completion_us\":"<<max_completion_us
        << ",\"send_stalls\":"<<send_stalls<<",\"max_hfp_check_us\":"<<max_hfp_check_us
        << ",\"pending_signal\":" << finished.pending_signal << ",\"pending_media\":" << finished.pending_media
        << ",\"pending_avrcp\":" << finished.pending_avrcp << "}" << std::endl;
}
}
void serve_audio(HANDLE stop,unsigned duration_seconds) {
    ComApartment apartment(COINIT_MULTITHREADED);
    HfpActivity hfp;
    bool was_hfp=false;
    auto last_format=service_profile();
    const auto started=std::chrono::steady_clock::now();
    std::jthread duration;
    if (duration_seconds) duration=std::jthread([&](std::stop_token token) {
        while (!token.stop_requested() && !stopped(stop)) {
            if (std::chrono::steady_clock::now()-started>=std::chrono::seconds(duration_seconds)) { SetEvent(stop); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    while (!stopped(stop)) {
        bool retry=true;
        try {
            const auto profile=service_profile();
            if (bluetooth_connected(target_device().address)) {
                const bool calling=hfp.active();
                if(calling!=was_hfp) {
                    was_hfp=calling;
                    std::osyncstream(std::cout)<<"{\"event\":\"hfp_state\",\"active\":"<<(calling?"true":"false")<<"}"<<std::endl;
                }
                if(calling) {
                    // Windows owns the unified endpoint's native HFP route.
                    if(WaitForSingleObject(stop,100)==WAIT_OBJECT_0) break;
                    continue;
                }
                const auto path=find_direct_pcm();
                if (path) {
                    DirectPcm pcm(*path);
                    transfer(pcm,stop,profile,last_format,hfp);
                    retry=false;
                }
            }
        } catch (const std::exception& error) {
            std::cerr << "{\"event\":\"service_audio_error\",\"message\":" << json_string(error.what()) << "}" << std::endl;
        }
        if (retry && WaitForSingleObject(stop,1000)==WAIT_OBJECT_0) break;
    }
}
}
