#pragma once
#include "lhdc_v5.hpp"
#include <functional>
namespace lhdc::avdtp {
enum class StreamState { idle,configured,opened,streaming };
class Stream {
public:
    using Exchange=std::function<Bytes(Signal,const Bytes&)>;
    Stream(Exchange exchange,std::uint8_t remote_seid,Bytes configuration);
    void configure();
    void open();
    void media_ready();
    void start();
    void suspend();
    void close();
    void abort();
    StreamState state() const { return state_; }
private:
    void empty_accept(Signal signal);
    Exchange exchange_;
    Bytes seid_,configuration_;
    StreamState state_=StreamState::idle;
    bool media_ready_=false;
    bool configuration_verified_=false;
};
}
