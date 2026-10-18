#include "signalling.hpp"
#include "json.hpp"
#include <chrono>
#include <iostream>
#include <syncstream>
namespace av=lhdc::avdtp;
namespace lhdc {
Signalling::Signalling(Transport& transport,Profile profile)
    :Signalling(profile,[&transport](auto bytes){transport.send(bytes);},
        [&transport](DWORD timeout){return transport.receive(LHDC_CHANNEL_SIGNAL,timeout);}) {}
void Signalling::transmit(const av::Bytes& bytes) {
    std::osyncstream(std::cout) << "{\"event\":\"tx\",\"sdu\":" << json_string(av::hex(bytes)) << "}\n";
    send_(bytes);
}
av::Message Signalling::receive_message(const av::Bytes& first) {
    av::Reassembler assembler;
    auto consume=[&](const av::Bytes& bytes) {
        std::osyncstream(std::cout)<<"{\"event\":\"rx\",\"sdu\":"<<json_string(av::hex(bytes))<<"}\n";
        return assembler.consume(bytes);
    };
    auto message=consume(first);
    // An idle poll may time out before a message starts. Once a fragment is
    // received, finish that message before handing the reader to a transaction.
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    while(!message) {
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
        if(remaining<=0) throw std::runtime_error("Incomplete AVDTP peer message timed out");
        try { message=consume(receive_(static_cast<DWORD>(remaining))); }
        catch(const WindowsError& error) {
            // A partial-message timeout is a failed connection, not an empty
            // idle poll: reusing it would lose the fragment boundary.
            throw std::runtime_error(std::string("Incomplete AVDTP peer message: ")+error.what());
        }
    }
    return std::move(*message);
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
    if (stop && (message.signal==av::Signal::close || message.signal==av::Signal::abort)) peer_released_=true;
    return stop;
}
av::Message Signalling::exchange(av::Signal signal,const av::Bytes& payload) {
    const auto label=label_; label_=static_cast<std::uint8_t>((label_+1)&15);
    std::osyncstream(std::cout)<<"{\"event\":\"avdtp_transaction_begin\",\"signal\":"<<unsigned(signal)
        <<",\"label\":"<<unsigned(label)<<"}"<<std::endl;
    const auto started=std::chrono::steady_clock::now();
    transmit(av::command(label,signal,payload));
    const auto sent=std::chrono::steady_clock::now();
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(8);
    while (std::chrono::steady_clock::now()<deadline) {
        const auto remaining=std::chrono::duration_cast<std::chrono::milliseconds>(deadline-std::chrono::steady_clock::now()).count();
        if(remaining<=0) break;
        auto response=receive_message(receive_(static_cast<DWORD>(remaining)));
        if (response.type==av::MessageType::command) {
            if (respond(response)) throw std::runtime_error("Peer stopped the stream during a transaction");
            continue;
        }
        if (response.label!=label || response.signal!=signal) throw std::runtime_error("Unexpected AVDTP response transaction");
        const auto received=std::chrono::steady_clock::now();
        std::osyncstream(std::cout)<<"{\"event\":\"avdtp_transaction_complete\",\"signal\":"<<unsigned(signal)
            <<",\"label\":"<<unsigned(label)<<",\"response_type\":"<<unsigned(response.type)
            <<",\"send_us\":"<<std::chrono::duration_cast<std::chrono::microseconds>(sent-started).count()
            <<",\"response_us\":"<<std::chrono::duration_cast<std::chrono::microseconds>(received-sent).count()
            <<",\"elapsed_us\":"<<std::chrono::duration_cast<std::chrono::microseconds>(received-started).count()<<"}"<<std::endl;
        return response;
    }
    throw std::runtime_error("AVDTP transaction timed out");
}
av::Bytes Signalling::accept(av::Signal signal,const av::Bytes& payload) {
    auto response=exchange(signal,payload);
    if (response.type!=av::MessageType::accept)
        throw std::runtime_error("AVDTP rejected signal "+std::to_string(unsigned(signal))+", type="+std::to_string(unsigned(response.type))+", payload="+av::hex(response.payload));
    return response.payload;
}
bool Signalling::poll_peer(DWORD timeout_ms) {
    av::Bytes first;
    try { first=receive_(timeout_ms); }
    catch(const WindowsError& error) {
        if(error.code==ERROR_TIMEOUT || error.code==ERROR_SEM_TIMEOUT) return false;
        throw;
    }
    const auto message=receive_message(first);
    if (message.type!=av::MessageType::command) throw std::runtime_error("Unexpected unsolicited AVDTP response");
    if (respond(message)) throw std::runtime_error("Peer requested stream stop, signal="+std::to_string(unsigned(message.signal)));
    return true;
}
}
