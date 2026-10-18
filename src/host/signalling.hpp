#pragma once
#include "avdtp/lhdc_v5.hpp"
#include "transport/windows.hpp"
#include <functional>
namespace lhdc {
class Signalling {
public:
    using Send=std::function<void(std::span<const std::uint8_t>)>;
    using Receive=std::function<avdtp::Bytes(DWORD)>;
    Signalling(Transport& transport,Profile profile);
    Signalling(Profile profile,Send send,Receive receive):send_(std::move(send)),receive_(std::move(receive)),profile_(profile){}
    avdtp::Bytes accept(avdtp::Signal signal,const avdtp::Bytes& payload={});
    void set_configuration(avdtp::Bytes selected) { selected_=std::move(selected); }
    void set_profile(Profile profile) { profile_=profile; }
    bool poll_peer(DWORD timeout_ms=10000);
    bool peer_released() const { return peer_released_; }
private:
    avdtp::Message receive_message(const avdtp::Bytes& first);
    void transmit(const avdtp::Bytes& bytes);
    bool respond(const avdtp::Message& message);
    avdtp::Message exchange(avdtp::Signal signal,const avdtp::Bytes& payload);
    Send send_;
    Receive receive_;
    Profile profile_;
    avdtp::Bytes selected_;
    std::uint8_t label_=0;
    bool peer_released_=false;
};
}
