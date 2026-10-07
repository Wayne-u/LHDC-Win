#include "signalling.hpp"
#include "json.hpp"
#include <chrono>
#include <iostream>
#include <syncstream>
namespace av=lhdc::avdtp;
namespace lhdc {
void Signalling::transmit(const av::Bytes& bytes) {
    std::osyncstream(std::cout) << "{\"event\":\"tx\",\"sdu\":" << json_string(av::hex(bytes)) << "}\n";
    transport_.send(bytes);
}
bool Signalling::respond(const av::Message& message) {
    av::Bytes payload;
    auto type=av::MessageType::accept;
    bool stop=false;
    if (message.signal==av::Signal::discover && message.payload.empty()) payload={static_cast<std::uint8_t>(selected_.empty()?4:6),0};
    else if ((message.signal==av::Signal::get_capabilities || message.signal==av::Signal::get_all_capabilities) && message.payload==av::Bytes{4}) payload=av::local_capabilities(profile_);
    else if (message.signal==av::Signal::get_configuration && message.payload==av::Bytes{4} && !selected_.empty()) payload=selected_;
    else if ((message.signal==av::Signal::suspend || message.signal==av::Signal::close || message.signal==av::Signal::abort) && message.payload==av::Bytes{4} && !selected_.empty()) stop=true;
    else {
        type=av::MessageType::reject; payload={0x19};
        if (message.signal==av::Signal::start || message.signal==av::Signal::suspend) payload={4,0x31};
        if (message.signal==av::Signal::set_configuration) payload={0,0x31};
    }
    auto bytes=av::command(message.label,message.signal,payload);
    bytes[0]|=static_cast<std::uint8_t>(type); transmit(bytes);
    return stop;
}
av::Message Signalling::exchange(av::Signal signal,const av::Bytes& payload) {
    const auto label=label_; label_=static_cast<std::uint8_t>((label_+1)&15);
    transmit(av::command(label,signal,payload));
    av::Reassembler assembler;
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    while (std::chrono::steady_clock::now()<deadline) {
        const auto raw=transport_.receive();
        std::osyncstream(std::cout) << "{\"event\":\"rx\",\"sdu\":" << json_string(av::hex(raw)) << "}\n";
        const auto response=assembler.consume(raw);
        if (!response) continue;
        if (response->type==av::MessageType::command) {
            if (respond(*response)) throw std::runtime_error("Peer stopped the stream during a transaction");
            continue;
        }
        if (response->label!=label || response->signal!=signal) throw std::runtime_error("Unexpected AVDTP response transaction");
        return *response;
    }
    throw std::runtime_error("AVDTP transaction timed out");
}
av::Bytes Signalling::accept(av::Signal signal,const av::Bytes& payload) {
    auto response=exchange(signal,payload);
    if (response.type!=av::MessageType::accept)
        throw std::runtime_error("AVDTP rejected signal "+std::to_string(unsigned(signal))+", type="+std::to_string(unsigned(response.type))+", payload="+av::hex(response.payload));
    return response.payload;
}
void Signalling::poll_peer() {
    const auto raw=transport_.receive();
    std::osyncstream(std::cout) << "{\"event\":\"rx\",\"sdu\":" << json_string(av::hex(raw)) << "}\n";
    const auto message=monitor_.consume(raw);
    if (!message) return;
    if (message->type!=av::MessageType::command) throw std::runtime_error("Unexpected AVDTP response while streaming");
    if (respond(*message)) throw std::runtime_error("Peer requested stream stop, signal="+std::to_string(unsigned(message->signal)));
}
}
