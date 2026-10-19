#include "media_session.hpp"
#include "json.hpp"
#include "media/packetizer.hpp"
#include <iostream>
#include <syncstream>
#include <algorithm>
#include <chrono>
namespace av=lhdc::avdtp;
namespace lhdc {
constexpr DWORD media_send_timeout_ms=500;
static void cleanup_error(const char* event,const std::exception& error) {
    std::osyncstream(std::cerr) << "{\"event\":" << json_string(event) << ",\"message\":" << json_string(error.what()) << "}" << std::endl;
}
MediaSession::MediaSession(std::uint64_t remote,Profile profile,bool control_volume)
    :transport_(remote),protocol_(transport_,profile),profile_(profile) {
    if (control_volume) volume_=std::make_unique<VolumeSync>(transport_,absolute_volume_enabled());
}
void MediaSession::discover() {
    transport_.open();
    const auto info=transport_.info();
    std::osyncstream(std::cout) << "{\"event\":\"channel_open\",\"channel_id\":1,\"in_mtu\":" << info.InMtu << ",\"out_mtu\":" << info.OutMtu << "}\n";
    const auto endpoints=av::endpoints(protocol_.accept(av::Signal::discover));
    std::string incompatible;
    for (const auto& endpoint:endpoints) {
        if (!endpoint.sink || endpoint.media_type!=0 || endpoint.in_use) continue;
        const auto caps=protocol_.accept(av::Signal::get_capabilities,{static_cast<std::uint8_t>(endpoint.seid<<2)});
        std::osyncstream(std::cout) << "{\"event\":\"peer_capabilities\",\"seid\":" << unsigned(endpoint.seid) << ",\"raw\":" << json_string(av::hex(caps)) << "}\n";
        bool v5=false;
        for (const auto& cap:av::capabilities(caps)) {
            const auto vendor=av::vendor_codec(cap);
            if (vendor && vendor->vendor_id==av::lhdc_v5_vendor && vendor->codec_id==av::lhdc_v5_codec) v5=true;
        }
        if (!v5) continue;
        try { av::select_configuration(caps,profile_); }
        catch (const std::runtime_error& error) { incompatible=error.what();continue; }
        peer_caps_=caps; remote_seid_=endpoint.seid; break;
    }
    if (!remote_seid_) throw std::runtime_error(incompatible.empty()?"No available LHDC V5 endpoint":"No compatible LHDC V5 endpoint: "+incompatible);
}
bool MediaSession::configured_for(Profile profile) const {
    return stream_ && stream_->state()==av::StreamState::configured &&
        stream_->configuration_readback()!=av::ConfigurationReadback::unverified &&
        profile.sample_rate==profile_.sample_rate && profile.bits==profile_.bits && profile.kbps==profile_.kbps;
}
void MediaSession::configure(Profile profile) {
    if (configured_for(profile)) return;
    const auto selected=av::select_configuration(peer_caps_,profile);
    if (stream_) {
        if (stream_->state()!=av::StreamState::configured) throw std::logic_error("Configuration changes require a closed media channel");
        stream_->abort(); protocol_.set_configuration({}); stream_.reset();
    }
    profile_=profile; protocol_.set_profile(profile);
    stream_.emplace([&](av::Signal signal,const av::Bytes& payload){return protocol_.accept(signal,payload);},remote_seid_,selected,peer_caps_);
    stream_->configure(); protocol_.set_configuration(selected);
    const bool exact=stream_->configuration_readback()==av::ConfigurationReadback::exact;
    const auto event=exact?"configuration_verified":"configuration_accepted";
    const auto readback=exact?"exact":"capability_echo";
    std::osyncstream(std::cout) << "{\"event\":" << json_string(event) << ",\"seid\":" << unsigned(remote_seid_) << ",\"raw\":" << json_string(av::hex(selected))
        << ",\"configuration_readback\":" << json_string(readback) << ",\"readback_verified\":" << (exact?"true":"false")
        << ",\"sample_rate\":" << profile_.sample_rate << ",\"bits\":" << profile_.bits << ",\"target_kbps\":" << profile_.kbps << ",\"stream_started\":false}\n";
}
void MediaSession::open_media() {
    if (!stream_) throw std::logic_error("Media Open requires a configured stream");
    stream_->open();
    std::osyncstream(std::cout)<<"{\"event\":\"media_channel_open_begin\",\"channel_id\":2}"<<std::endl;
    const auto opening=std::chrono::steady_clock::now();
    transport_.open(LHDC_CHANNEL_MEDIA);
    media_info_=transport_.info(LHDC_CHANNEL_MEDIA);
    const auto opened=std::chrono::steady_clock::now();
    std::osyncstream(std::cout) << "{\"event\":\"media_channel_open\",\"channel_id\":2,\"in_mtu\":" << media_info_.InMtu << ",\"out_mtu\":" << media_info_.OutMtu
        <<",\"elapsed_us\":"<<std::chrono::duration_cast<std::chrono::microseconds>(opened-opening).count()<<"}"<<std::endl;
    if (media_info_.OutMtu<=media_header_bytes) throw std::runtime_error("Media channel MTU is smaller than media headers");
}
void MediaSession::poll_idle() {
    check();
    // The service thread owns reads until Start; the streaming monitor takes
    // over afterwards. No background reader may race a configuration exchange.
    protocol_.poll_peer(5);
}
void MediaSession::close_prepared() {
    stream_->close();protocol_.set_configuration({});
    transport_.close(LHDC_CHANNEL_MEDIA);transport_.close();
    for (const auto channel:{LHDC_CHANNEL_SIGNAL,LHDC_CHANNEL_MEDIA}) {
        const auto state=transport_.info(channel);
        if (state.Connected || state.PendingRequests) throw std::runtime_error("Compatibility probe left an active channel or pending request");
    }
    finished_=true;
}
void MediaSession::start(std::uint32_t block_samples) {
    stream_->media_ready(); stream_->start();
    std::osyncstream(std::cout) << "{\"event\":\"stream_started\",\"sample_rate\":" << profile_.sample_rate << ",\"bits\":" << profile_.bits
        << ",\"target_kbps\":" << profile_.kbps << ",\"block_samples\":" << block_samples << "}\n";
    monitor_=std::thread([&] {
        try {
            while (!stop_.load()) {
                try { protocol_.poll_peer(); }
                catch (const WindowsError& error) {
                    if (stop_.load() && error.code==ERROR_OPERATION_ABORTED) break;
                    throw;
                }
            }
        } catch (...) { monitor_error_=std::current_exception(); signal_failed_.store(true); }
    });
}
void MediaSession::check() {
    if (volume_) volume_->check();
    if (signal_failed_.load()) throw std::runtime_error("Signalling monitor failed during media transmission");
}
bool MediaSession::supports_bitrate(Profile profile) const {
    const auto range=av::v5_capabilities(av::local_capabilities(profile_));
    return profile.sample_rate==profile_.sample_rate && profile.bits==profile_.bits &&
        profile.kbps>=range.min_kbps && profile.kbps<=range.max_kbps &&
        encoded_frame_bytes(profile)<=media_info_.OutMtu-media_header_bytes;
}
SendTiming MediaSession::send(std::span<const std::uint8_t> sdu) {
    check();
    SendTiming timing;
    // Submit in RTP order and retire the oldest first. The shared window bounds
    // user buffers and driver requests while Bluetooth completions are delayed.
    if (pending_media_.size()==LHDC_MEDIA_SEND_WINDOW) {
        // Include time already spent in the send window. A stalled media
        // channel must be rebuilt instead of feeding seconds-old RTP later.
        timing=pending_media_.front()->wait(media_send_timeout_ms);
        pending_media_.pop_front();
    }
    auto request=transport_.begin_send(sdu,LHDC_CHANNEL_MEDIA);
    timing.submit_us=request->timing().submit_us;
    timing.pending=timing.pending || request->timing().pending;
    pending_media_.push_back(std::move(request));
    for (const auto& pending:pending_media_)
        timing.pending_age_us=std::max(timing.pending_age_us,pending->pending_age_us());
    sent_bytes_+=sdu.size();
    return timing;
}
void MediaSession::stop_monitor() {
    stop_.store(true);
    if (!monitor_.joinable()) return;
    try { transport_.cancel_pending(); } catch (const std::exception& error) { cleanup_error("cancel_failed",error); }
    monitor_.join();
}
SessionCompletion MediaSession::finish() {
    while (!pending_media_.empty()) {
        pending_media_.front()->wait(media_send_timeout_ms);
        pending_media_.pop_front();
    }
    volume_.reset();
    stop_monitor();
    if (monitor_error_) std::rethrow_exception(monitor_error_);
    const auto stats=transport_.info(LHDC_CHANNEL_MEDIA);
    if (stats.PendingRequests || stats.CompletedBytes-media_info_.CompletedBytes!=sent_bytes_ || stats.SubmittedBytes-media_info_.SubmittedBytes!=sent_bytes_)
        throw std::runtime_error("Media completion totals differ from submitted SDUs");
    stream_->suspend(); stream_->close(); protocol_.set_configuration({});
    transport_.close(LHDC_CHANNEL_MEDIA); transport_.close();
    const auto final_media=transport_.info(LHDC_CHANNEL_MEDIA),final_signal=transport_.info();
    const auto final_avrcp=transport_.info(LHDC_CHANNEL_AVRCP);
    if (final_media.PendingRequests || final_signal.PendingRequests || final_avrcp.PendingRequests ||
        final_media.Connected || final_signal.Connected || final_avrcp.Connected)
        throw std::runtime_error("Stream closed with an active channel or pending request");
    finished_=true;
    return {sent_bytes_,final_signal.PendingRequests,final_media.PendingRequests,final_avrcp.PendingRequests};
}
MediaSession::~MediaSession() {
    volume_.reset();
    stop_monitor();
    pending_media_.clear();
    if (finished_) return;
    if (monitor_error_) {
        try { std::rethrow_exception(monitor_error_); } catch (const std::exception& error) { cleanup_error("signalling_monitor_failed",error); }
    }
    if (stream_ && !protocol_.peer_released()) {
        try { stream_->abort(); } catch (const std::exception& error) { cleanup_error("abort_failed",error); }
    }
    for (const auto channel:{LHDC_CHANNEL_MEDIA,LHDC_CHANNEL_SIGNAL}) {
        try { transport_.close(channel); } catch (const std::exception& error) { cleanup_error("channel_cleanup_failed",error); }
    }
}
void inspect_playback_compatibility(Profile profile) {
    av::Bytes caps;
    ULONG mtu=0;
    av::ConfigurationReadback readback=av::ConfigurationReadback::unverified;
    {
        MediaSession session(target_device().address,profile,false);
        session.discover();session.configure(profile);session.open_media();
        caps=session.peer_capabilities();mtu=session.media_info().OutMtu;
        readback=session.configuration_readback();
        if (encoded_frame_bytes(profile)>mtu-media_header_bytes) throw std::runtime_error("Selected bitrate exceeds the negotiated media MTU");
        session.close_prepared();
    }
    const bool exact=readback==av::ConfigurationReadback::exact;
    std::cout<<"{\"event\":\"playback_compatibility\",\"compatible\":true,\"peer_capabilities\":"<<json_string(av::hex(caps))
        <<",\"configuration_readback\":"<<json_string(exact?"exact":"capability_echo")<<",\"readback_verified\":"<<(exact?"true":"false")
        <<",\"media_mtu\":"<<mtu<<",\"stream_started\":false}"<<std::endl;
}
}
