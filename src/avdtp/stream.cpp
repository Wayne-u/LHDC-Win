#include "stream.hpp"
#include <stdexcept>
namespace lhdc::avdtp {
Stream::Stream(Exchange exchange,std::uint8_t seid,Bytes configuration)
    :exchange_(std::move(exchange)),configuration_(std::move(configuration)) {
    if (!seid || seid>62) throw std::invalid_argument("Invalid remote stream endpoint");
    seid_={static_cast<std::uint8_t>(seid<<2)};
}
void Stream::configure() {
    if (state_!=StreamState::idle) throw std::logic_error("Stream is already configured");
    Bytes request{seid_[0],4}; // local source SEID 1
    request.insert(request.end(),configuration_.begin(),configuration_.end());
    const auto accepted=exchange_(Signal::set_configuration,request);
    state_=StreamState::configured;
    if (!accepted.empty()) throw std::runtime_error("Unexpected SetConfiguration Accept payload");
    verify_configuration(configuration_,exchange_(Signal::get_configuration,seid_));
    configuration_verified_=true;
}
void Stream::empty_accept(Signal signal) {
    if (!exchange_(signal,seid_).empty()) throw std::runtime_error("Unexpected stream Accept payload");
}
void Stream::open() {
    if (state_!=StreamState::configured || !configuration_verified_) throw std::logic_error("Open requires a verified configuration");
    empty_accept(Signal::open); state_=StreamState::opened;
}
void Stream::media_ready() {
    if (state_!=StreamState::opened) throw std::logic_error("Media transport requires Open Accept");
    media_ready_=true;
}
void Stream::start() {
    if (state_!=StreamState::opened || !media_ready_) throw std::logic_error("Start requires an open media channel");
    empty_accept(Signal::start); state_=StreamState::streaming;
}
void Stream::suspend() {
    if (state_!=StreamState::streaming) throw std::logic_error("Suspend requires a streaming endpoint");
    empty_accept(Signal::suspend); state_=StreamState::opened;
}
void Stream::close() {
    if (state_!=StreamState::opened) throw std::logic_error("Close requires an opened, suspended endpoint");
    empty_accept(Signal::close); state_=StreamState::idle; media_ready_=false; configuration_verified_=false;
}
void Stream::abort() {
    if (state_==StreamState::idle) return;
    empty_accept(Signal::abort); state_=StreamState::idle; media_ready_=false; configuration_verified_=false;
}
}
