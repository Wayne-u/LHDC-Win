#pragma once
#include "lhdc_v5.hpp"
#include <functional>
namespace lhdc::avdtp {
enum class StreamState { idle,configured,opened,streaming };
class Stream {
public:
    using Exchange=std::function<Bytes(Signal,const Bytes&)>;
    Stream(Exchange exchange,std::uint8_t remote_seid,Bytes configuration,Bytes peer_capabilities);
    void configure();
    void open();
    void media_ready();
    void start();
    void suspend();
    void close();
    void abort();
    StreamState state() const { return state_; }
    ConfigurationReadback configuration_readback() const { return readback_; }
private:
    void empty_accept(Signal signal);
    Exchange exchange_;
    Bytes seid_,configuration_,peer_capabilities_;
    StreamState state_=StreamState::idle;
    bool media_ready_=false;
    ConfigurationReadback readback_=ConfigurationReadback::unverified;
};
}
