#include "media_session.hpp"
#include "json.hpp"
#include "media/packetizer.hpp"
#include <iostream>
#include <syncstream>
#include <algorithm>
namespace av=lhdc::avdtp;
namespace lhdc {
static void cleanup_error(const char* event,const std::exception& error) {
    std::osyncstream(std::cerr) << "{\"event\":" << json_string(event) << ",\"message\":" << json_string(error.what()) << "}" << std::endl;
}
void MediaSession::discover() {
    volume_=std::make_unique<VolumeSync>(transport_);
    transport_.open();
    const auto info=transport_.info();
    std::osyncstream(std::cout) << "{\"event\":\"channel_open\",\"channel_id\":1,\"in_mtu\":" << info.InMtu << ",\"out_mtu\":" << info.OutMtu << "}\n";
    const auto endpoints=av::endpoints(protocol_.accept(av::Signal::discover));
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
        av::v5_capabilities(caps);
        peer_caps_=caps; remote_seid_=endpoint.seid; break;
    }
    if (!remote_seid_) throw std::runtime_error("No available LHDC V5 endpoint");
}
void MediaSession::configure(Profile profile) {
    const auto selected=av::select_configuration(peer_caps_,profile);
    profile_=profile; protocol_.set_profile(profile);
    stream_.emplace([&](av::Signal signal,const av::Bytes& payload){return protocol_.accept(signal,payload);},remote_seid_,selected);
    stream_->configure(); protocol_.set_configuration(selected);
    std::osyncstream(std::cout) << "{\"event\":\"configuration_verified\",\"seid\":" << unsigned(remote_seid_) << ",\"raw\":" << json_string(av::hex(selected))
        << ",\"sample_rate\":" << profile_.sample_rate << ",\"bits\":" << profile_.bits << ",\"target_kbps\":" << profile_.kbps << ",\"stream_started\":false}\n";
    stream_->open(); transport_.open(LHDC_CHANNEL_MEDIA);
    media_info_=transport_.info(LHDC_CHANNEL_MEDIA);
    std::osyncstream(std::cout) << "{\"event\":\"media_channel_open\",\"channel_id\":2,\"in_mtu\":" << media_info_.InMtu << ",\"out_mtu\":" << media_info_.OutMtu << "}\n";
    if (media_info_.OutMtu<=media_header_bytes) throw std::runtime_error("Media channel MTU is smaller than media headers");
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
                    if (error.code==ERROR_SEM_TIMEOUT || error.code==ERROR_TIMEOUT) continue;
                    throw;
                }
            }
        } catch (...) { monitor_error_=std::current_exception(); signal_failed_.store(true); }
    });
}
void MediaSession::check() {
    volume_->check();
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
        timing=pending_media_.front()->wait();
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
        pending_media_.front()->wait();
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
    if (stream_) {
        try { stream_->abort(); } catch (const std::exception& error) { cleanup_error("abort_failed",error); }
    }
    for (const auto channel:{LHDC_CHANNEL_MEDIA,LHDC_CHANNEL_SIGNAL}) {
        try { transport_.close(channel); } catch (const std::exception& error) { cleanup_error("channel_cleanup_failed",error); }
    }
}
}
