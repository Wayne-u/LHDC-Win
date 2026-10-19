#include "stream.hpp"
#include "progress_log.hpp"
#include "status.hpp"
#include "codec/bitrate_control.hpp"
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
#include "control/rfcomm.hpp"
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
std::uint64_t clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
bool stopped(HANDLE stop) {
    const auto result=WaitForSingleObject(stop,0);
    if (result!=WAIT_OBJECT_0 && result!=WAIT_TIMEOUT) throw std::runtime_error("Service stop wait failed");
    return result==WAIT_OBJECT_0;
}
void transfer(DirectPcm& pcm,HANDLE stop,Profile profile,const HfpActivity& hfp) {
    AudioTask priority; Pacer pacer;
    auto quality=profile;
    const auto revision=capability_revision();
    // Prepare and verify the requested codec while idle. Keep one signal
    // reader, and open media only after an actual render stream arrives.
    const auto control_started=std::chrono::steady_clock::now();
    MediaSession session(target_device().address,profile);
    session.discover();
    const auto& caps=session.peer_capabilities();
    const auto saved=RegSetKeyValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\LHDC-Win",L"PeerCapabilities",REG_BINARY,caps.data(),static_cast<DWORD>(caps.size()));
    if (saved!=ERROR_SUCCESS) throw std::runtime_error("Peer capability cache write failed, Win32="+std::to_string(saved));
    const auto& instance=target_device().instance;
    const auto saved_instance=RegSetKeyValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\LHDC-Win",L"PeerDeviceInstance",REG_SZ,instance.c_str(),static_cast<DWORD>((instance.size()+1)*sizeof(wchar_t)));
    if (saved_instance!=ERROR_SUCCESS) throw WindowsError("Peer device cache write",saved_instance);
    const auto saved_revision=RegSetKeyValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\LHDC-Win",L"PeerCapabilityRevision",REG_DWORD,&revision,sizeof(revision));
    if(saved_revision!=ERROR_SUCCESS) throw WindowsError("Peer capability revision write",saved_revision);
    session.configure(profile);
    const auto control_ready=std::chrono::steady_clock::now();
    const auto control_ms=std::chrono::duration_cast<std::chrono::milliseconds>(control_ready-control_started).count();
    std::osyncstream(std::cout)<<"{\"event\":\"media_control_ready\",\"prepare_ms\":"<<control_ms
        <<",\"configuration_prepared\":true,\"sample_rate\":"<<profile.sample_rate<<",\"bits\":"<<profile.bits<<",\"target_kbps\":"<<profile.kbps<<"}"<<std::endl;
    LHDC_PCM_STATE state{};
    auto next_hfp_check=std::chrono::steady_clock::now();
    while (!stopped(stop)) {
        session.check();
        if (!same(quality,service_profile()) || revision!=capability_revision()) return;
        state=pcm.state();
        if (state.Running) break;
        if(std::chrono::steady_clock::now()>=next_hfp_check) {
            if(hfp.active() || !bluetooth_connected(target_device().address)) return;
            next_hfp_check=std::chrono::steady_clock::now()+std::chrono::milliseconds(100);
        }
        session.poll_idle();
    }
    if (stopped(stop)) return;
    session.check();
    if (state.Channels!=2 || state.BlockAlign!=2*(state.Bits/8)) throw std::runtime_error("Direct PCM requires interleaved stereo");
    // Prepare from the user's requested format, not transient idle probes.
    // Reconfigure if this render epoch actually requires a different codec.
    profile=follow_pcm(quality,state.SampleRate,state.Bits,state.FloatingPoint!=0);
    const auto preparing=std::chrono::steady_clock::now();
    std::osyncstream(std::cout)<<"{\"event\":\"direct_audio_prepare\",\"epoch\":"<<state.Epoch
        <<",\"input_sample_rate\":"<<state.SampleRate<<",\"input_bits\":"<<state.Bits
        <<",\"idle_ms\":"<<std::chrono::duration_cast<std::chrono::milliseconds>(preparing-control_ready).count()<<"}"<<std::endl;
    const bool configuration_reused=session.configured_for(profile);
    session.configure(profile);
    session.open_media();
    const auto configured_at=std::chrono::steady_clock::now();
    const auto media_mtu=session.media_info().OutMtu;
    const auto saved_mtu=RegSetKeyValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\LHDC-Win",L"PeerMediaMtu",REG_DWORD,&media_mtu,sizeof(media_mtu));
    if(saved_mtu!=ERROR_SUCCESS) throw WindowsError("Media MTU cache write",saved_mtu);
    auto adaptive=adaptive_bitrate_enabled();
    BitrateControl bitrate_control(profile,adaptive,clock_ms());
    auto encoding_profile=profile;
    encoding_profile.kbps=bitrate_control.bitrate();
    Encoder encoder(encoding_profile,session.media_info().OutMtu-media_header_bytes);
    Packetizer packetizer(session.media_info().OutMtu,encoder.block_samples());
    const std::size_t byte_rate=static_cast<std::size_t>(profile.sample_rate)*2*(profile.bits/8);
    const auto preroll_bytes=byte_rate*pcm_preroll_ms/1000;
    // Start can wait for the peer while Windows continues producing PCM.
    // Complete that exchange before discarding stale startup data and preroll.
    const auto configured=pcm.state();
    if(stopped(stop) || !configured.Running || configured.Epoch!=state.Epoch || hfp.active() ||
        !same(quality,service_profile()) || revision!=capability_revision()) return;
    const auto start_requested_at=std::chrono::steady_clock::now();
    session.start(encoder.block_samples());
    const auto stream_started_at=std::chrono::steady_clock::now();
    // Preserve the beginning of a newly started render stream. Discard only
    // stale backlog accumulated while negotiating or restarting the encoder.
    const auto pending=pcm.state();
    if(!pending.Running || pending.Epoch!=state.Epoch) return;
    const bool reset_startup=pending.DroppedBytes || pending.BufferedBytes>static_cast<std::uint64_t>(pending.SampleRate)*pending.BlockAlign/10;
    std::osyncstream(std::cout)<<"{\"event\":\"media_startup_complete\",\"epoch\":"<<state.Epoch
        <<",\"setup_ms\":"<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-preparing).count()
        <<",\"control_prepare_ms\":"<<control_ms
        <<",\"configuration_reused\":"<<(configuration_reused?"true":"false")
        <<",\"configure_ms\":"<<std::chrono::duration_cast<std::chrono::milliseconds>(configured_at-preparing).count()
        <<",\"pre_start_ms\":"<<std::chrono::duration_cast<std::chrono::milliseconds>(start_requested_at-configured_at).count()
        <<",\"start_ms\":"<<std::chrono::duration_cast<std::chrono::milliseconds>(stream_started_at-start_requested_at).count()
        <<",\"startup_reset_requested\":"<<(reset_startup?"true":"false")
        <<",\"buffered_bytes\":"<<pending.BufferedBytes<<",\"kernel_dropped_bytes\":"<<pending.DroppedBytes<<"}"<<std::endl;
    if (reset_startup) pcm.reset();
    PcmReader reader([&](std::size_t count){return pcm.read(count);},[&]{pcm.reset();},state,profile);
    auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
    auto started=std::chrono::steady_clock::now();
    std::uint64_t frames=0,packets=0,bytes=0,nonzero_blocks=0;
    std::uint64_t discarded_frames=0,encoder_discarded_samples=0;
    std::uint64_t send_stalls=0;
    std::int64_t max_hfp_check_us=0;
    std::int64_t max_send_us=0,max_submit_us=0,max_io_wait_us=0,max_completion_us=0,max_pending_age_us=0,max_encode_us=0,max_wait_us=0,max_report_us=0,max_profile_us=0,max_state_us=0,max_log_us=0;
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
    // Dropped PCM is already removed by the reader. Resume at real time;
    // backdating by queued samples would burst old packets into the headset.
    auto pace_origin=[&] {
        return std::chrono::steady_clock::now()-std::chrono::nanoseconds(frames*encoder.block_samples()*1000000000ull/profile.sample_rate);
    };
    started=pace_origin(); auto report=std::chrono::steady_clock::now();
    std::vector<std::uint8_t> block(encoder.block_bytes());
    bitrate_control=BitrateControl(profile,adaptive,clock_ms());
    publish_active_bitrate(encoder.bitrate());
    std::osyncstream(std::cout) << "{\"event\":\"direct_audio_started\",\"source\":\"WaveRT\",\"sample_rate\":" << profile.sample_rate << ",\"bits\":" << profile.bits << ",\"kbps\":" << encoder.bitrate()
        << ",\"ceiling_kbps\":"<<profile.kbps<<",\"adaptive_bitrate\":"<<(adaptive?"true":"false")
        << ",\"preroll_ms\":"<<pcm_preroll_ms
        << ",\"input_sample_rate\":" << state.SampleRate << ",\"input_bits\":" << state.Bits << ",\"input_float\":" << (state.FloatingPoint?"true":"false")
        << ",\"media_send_window\":" << LHDC_MEDIA_SEND_WINDOW << ",\"resampling\":false}" << std::endl;
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
        std::uint64_t block_discarded_frames=0;
        if (!reader.pop(block,&block_discarded_frames)) {
            const auto empty=reader.stats();
            if(empty.finished) break;
            if (std::chrono::steady_clock::now()-empty.last_input>std::chrono::milliseconds(250)) throw std::runtime_error("Direct PCM producer stalled");
            if (WaitForSingleObject(stop,2)==WAIT_OBJECT_0) break;
            continue;
        }
        if(block_discarded_frames!=discarded_frames) {
            // PCM time continues across dropped samples. Keep RTP on that
            // timeline and discard any encoder frame buffered before the gap.
            const auto buffered=encoder.buffered_samples();
            const auto skipped=block_discarded_frames-discarded_frames+buffered;
            packetizer.skip_samples(skipped);
            encoding_profile.kbps=encoder.bitrate();
            encoder=Encoder(encoding_profile,session.media_info().OutMtu-media_header_bytes);
            encoder_discarded_samples+=buffered;
            discarded_frames=block_discarded_frames;
            started=pace_origin();
            std::osyncstream(std::cout)<<"{\"event\":\"media_timeline_resync\",\"skipped_samples\":"<<skipped
                <<",\"encoder_discarded_samples\":"<<buffered<<"}"<<std::endl;
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
        max_pending_age_us=std::max(max_pending_age_us,timing.pending_age_us);
        constexpr std::int64_t send_stall_warning_us=100000;
        if(send_us>=send_stall_warning_us) {
            ++send_stalls;
            std::osyncstream(std::cout)<<"{\"event\":\"media_send_stall\",\"send_us\":"<<send_us
                <<",\"packet\":"<<(packets+1)<<",\"elapsed_ms\":"<<elapsed_us(started)/1000
                <<",\"submit_us\":"<<timing.submit_us<<",\"io_wait_us\":"<<timing.wait_us<<",\"completion_us\":"<<timing.completion_us
                <<",\"pending_age_us\":"<<timing.pending_age_us<<",\"io_pending\":"<<(timing.pending?"true":"false")<<"}"<<std::endl;
        }
        frames+=packet.frames; ++packets; bytes+=sdu.size();
        if (std::chrono::steady_clock::now()-report>=std::chrono::seconds(1)) {
            const auto report_started=std::chrono::steady_clock::now();
            const auto requested=service_profile();
            if (requested.sample_rate!=quality.sample_rate || requested.bits!=quality.bits || revision!=capability_revision()) break;
            const auto requested_adaptive=adaptive_bitrate_enabled();
            if (!same(quality,requested) || requested_adaptive!=adaptive) {
                if(requested.kbps<quality.kbps && (reader.stats().recoveries()!=0 || send_stalls!=0)) {
                    std::osyncstream(std::cout)<<"{\"event\":\"media_session_restart\",\"reason\":\"lower_ceiling_after_congestion\",\"previous_ceiling_kbps\":"<<quality.kbps
                        <<",\"ceiling_kbps\":"<<requested.kbps<<"}"<<std::endl;
                    break;
                }
                const auto selected=follow_pcm(requested,state.SampleRate,state.Bits,state.FloatingPoint!=0);
                if (!session.supports_bitrate(selected)) break;
                adaptive=requested_adaptive;
                bitrate_control=BitrateControl(selected,adaptive,clock_ms());
                profile=selected; quality=requested;
                std::osyncstream(std::cout)<<"{\"event\":\"codec_bitrate_settings\",\"ceiling_kbps\":"<<profile.kbps
                    <<",\"adaptive_bitrate\":"<<(adaptive?"true":"false")<<"}"<<std::endl;
            }
            max_profile_us=std::max(max_profile_us,elapsed_us(report_started));
            const auto state_started=std::chrono::steady_clock::now();
            const auto capture=reader.stats();
            const auto& info=capture.state;
            max_state_us=std::max(max_state_us,elapsed_us(state_started));
            const auto log_started=std::chrono::steady_clock::now();
            std::ostringstream snapshot;
            snapshot << "{\"event\":\"direct_audio_progress\",\"kbps\":"<<encoder.bitrate()<<",\"packets\":" << packets << ",\"bytes\":" << bytes << ",\"nonzero_blocks\":" << nonzero_blocks
                << ",\"queue_ms\":" << 1000.0*capture.queue.queued/byte_rate << ",\"queue_limit_ms\":"<<pcm_queue_duration_ms<<",\"queue_high_water_bytes\":"<<capture.queue.high_water
                << ",\"buffered_bytes\":" << info.BufferedBytes << ",\"produced_bytes\":" << info.ProducedBytes
                << ",\"dropped_bytes\":" << info.DroppedBytes << ",\"dropped_bytes_total\":" << capture.dropped_bytes()+encoder_discarded_samples*2*(profile.bits/8) << ",\"overrun_recoveries\":" << capture.overrun_recoveries << ",\"latency_recoveries\":" << capture.latency_recoveries
                << ",\"timeline_skipped_samples\":"<<discarded_frames+encoder_discarded_samples
                << ",\"encoder_discarded_pcm_bytes_total\":"<<encoder_discarded_samples*2*(profile.bits/8)
                << ",\"kernel_dropped_bytes_total\":"<<capture.kernel_dropped_bytes<<",\"discarded_pcm_bytes_total\":"<<capture.discarded_bytes
                << ",\"max_send_us\":" << max_send_us << ",\"max_submit_us\":"<<max_submit_us<<",\"max_io_wait_us\":"<<max_io_wait_us<<",\"max_completion_us\":"<<max_completion_us
                << ",\"pending_age_us\":"<<timing.pending_age_us<<",\"max_pending_age_us\":"<<max_pending_age_us
                << ",\"send_stalls\":"<<send_stalls<<",\"max_hfp_check_us\":"<<max_hfp_check_us<<",\"max_read_gap_us\":" << capture.max_read_gap_us << ",\"max_read_us\":" << capture.max_read_us
                << ",\"max_encode_us\":" << max_encode_us << ",\"max_wait_us\":" << max_wait_us << ",\"max_report_us\":" << max_report_us << ",\"max_profile_us\":" << max_profile_us << ",\"max_state_us\":" << max_state_us << ",\"max_log_us\":" << max_log_us << "}";
            progress.submit(snapshot.str());
            max_log_us=std::max(max_log_us,elapsed_us(log_started));
            max_report_us=std::max(max_report_us,elapsed_us(report_started));
            report=std::chrono::steady_clock::now();
        }
        const auto feedback=reader.stats();
        const auto queue_ms=1000.0*feedback.queue.queued/byte_rate;
        const auto desired=bitrate_control.observe(clock_ms(),queue_ms,feedback.dropped_bytes(),timing.pending_age_us/1000.0);
        if(desired!=encoder.bitrate()) {
            const auto previous=encoder.bitrate();
            encoder.set_bitrate(desired);
            publish_active_bitrate(desired);
            std::osyncstream(std::cout)<<"{\"event\":\"codec_bitrate_changed\",\"previous_kbps\":"<<previous
                <<",\"kbps\":"<<desired<<",\"adaptive_bitrate\":"<<(adaptive?"true":"false")
                <<",\"queue_ms\":"<<queue_ms<<",\"pending_age_us\":"<<timing.pending_age_us<<",\"dropped_bytes_total\":"<<feedback.dropped_bytes()<<",\"restart\":false}"<<std::endl;
        }
    }
    reader.stop();
    const auto capture=reader.stats();
    progress.finish();
    const auto finished=session.finish();
    std::osyncstream(std::cout) << "{\"event\":\"direct_audio_complete\",\"packets\":" << packets << ",\"bytes\":" << bytes << ",\"completed_bytes\":" << finished.bytes
        << ",\"nonzero_blocks\":" << nonzero_blocks << ",\"dropped_bytes_total\":" << capture.dropped_bytes()+encoder_discarded_samples*2*(profile.bits/8) << ",\"overrun_recoveries\":" << capture.overrun_recoveries << ",\"latency_recoveries\":" << capture.latency_recoveries
        << ",\"timeline_skipped_samples\":"<<discarded_frames+encoder_discarded_samples
        << ",\"encoder_discarded_pcm_bytes_total\":"<<encoder_discarded_samples*2*(profile.bits/8)
        << ",\"kernel_dropped_bytes_total\":"<<capture.kernel_dropped_bytes<<",\"discarded_pcm_bytes_total\":"<<capture.discarded_bytes<<",\"queue_limit_ms\":"<<pcm_queue_duration_ms<<",\"queue_high_water_bytes\":"<<capture.queue.high_water
        << ",\"max_read_gap_us\":"<<capture.max_read_gap_us
        << ",\"max_send_us\":"<<max_send_us<<",\"max_submit_us\":"<<max_submit_us<<",\"max_io_wait_us\":"<<max_io_wait_us<<",\"max_completion_us\":"<<max_completion_us
        << ",\"max_pending_age_us\":"<<max_pending_age_us
        << ",\"send_stalls\":"<<send_stalls<<",\"max_hfp_check_us\":"<<max_hfp_check_us
        << ",\"pending_signal\":" << finished.pending_signal << ",\"pending_media\":" << finished.pending_media
        << ",\"pending_avrcp\":" << finished.pending_avrcp << "}" << std::endl;
}
}
void serve_audio(HANDLE stop,unsigned duration_seconds) {
    ComApartment apartment(COINIT_MULTITHREADED);
    std::unique_ptr<HfpActivity> hfp;
    bool was_hfp=false;
    publish_active_bitrate(0);
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
            const bool connected=bluetooth_connected(target_device().address);
            sync_audio_connection(connected);
            if (connected) {
                claim_avrcp_controller();
                // Bluetooth devnodes can appear after the automatic service
                // starts. Initialize within the retry loop and rebuild watches
                // after a disconnect so they belong to the current endpoints.
                if (!hfp) hfp=std::make_unique<HfpActivity>();
                const bool calling=hfp->active();
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
                    transfer(pcm,stop,profile,*hfp);
                    retry=false;
                }
            } else {
                hfp.reset();
                was_hfp=false;
            }
        } catch (const std::exception& error) {
            hfp.reset();
            was_hfp=false;
            std::cerr << "{\"event\":\"service_audio_error\",\"message\":" << json_string(error.what()) << "}" << std::endl;
        }
        publish_active_bitrate(0);
        if (retry && WaitForSingleObject(stop,1000)==WAIT_OBJECT_0) break;
    }
    publish_active_bitrate(0);
}
}
