#pragma once
#include "avdtp/lhdc_v5.hpp"
#include "transport/windows.hpp"
namespace lhdc {
class Signalling {
public:
    Signalling(Transport& transport,Profile profile):transport_(transport),profile_(profile){}
    avdtp::Bytes accept(avdtp::Signal signal,const avdtp::Bytes& payload={});
    void set_configuration(avdtp::Bytes selected) { selected_=std::move(selected); }
    void set_profile(Profile profile) { profile_=profile; }
    void poll_peer();
private:
    void transmit(const avdtp::Bytes& bytes);
    bool respond(const avdtp::Message& message);
    avdtp::Message exchange(avdtp::Signal signal,const avdtp::Bytes& payload);
    Transport& transport_;
    Profile profile_;
    avdtp::Bytes selected_;
    avdtp::Reassembler monitor_;
    std::uint8_t label_=0;
};
}
