#pragma once
#include "signalling.hpp"
#include "avdtp/stream.hpp"
#include <optional>
#include <atomic>
#include <thread>
#include <memory>
#include "audio/volume_sync.hpp"
namespace lhdc {
struct SessionCompletion { std::uint64_t bytes; ULONG pending_signal,pending_media,pending_avrcp; };
class MediaSession {
public:
    MediaSession(std::uint64_t remote,Profile profile):transport_(remote),protocol_(transport_,profile),profile_(profile){}
    ~MediaSession();
    MediaSession(const MediaSession&)=delete;
    void open();
    void discover();
    void configure(Profile profile);
    const avdtp::Bytes& peer_capabilities() const { return peer_caps_; }
    void start(std::uint32_t block_samples);
    void check();
    SendTiming send(std::span<const std::uint8_t> sdu);
    SessionCompletion finish();
    const LHDC_INFO& media_info() const { return media_info_; }
private:
    void stop_monitor();
    Transport transport_;
    std::unique_ptr<VolumeSync> volume_;
    Signalling protocol_;
    Profile profile_;
    avdtp::Bytes peer_caps_;
    std::uint8_t remote_seid_=0;
    std::optional<avdtp::Stream> stream_;
    LHDC_INFO media_info_{};
    std::thread monitor_;
    std::atomic<bool> stop_=false,signal_failed_=false;
    std::exception_ptr monitor_error_;
    std::uint64_t sent_bytes_=0;
    bool finished_=false;
};
}
