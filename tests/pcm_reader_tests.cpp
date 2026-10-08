#include "audio/pcm_reader.hpp"
#include "host/pacer.hpp"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
using Clock=std::chrono::steady_clock;
static void require(bool value,const char* message) { if(!value) throw std::runtime_error(message); }
class Source {
public:
    Source(unsigned rate,unsigned bits):rate_(rate),align_(2*(bits/8)) {
        initial.SampleRate=rate;initial.Bits=bits;initial.Channels=2;initial.BlockAlign=align_;initial.Epoch=1;initial.Running=1;
    }
    LHDC_PCM_STATE initial{};
    std::atomic<unsigned> reads=0,resets=0;
    lhdc::DirectPcmChunk read(std::size_t bytes) {
        ++reads;
        const auto produced=produced_frames();
        const auto capacity=rate_/5;
        if(produced-cursor_>capacity) {
            dropped_+=(produced-cursor_-capacity)*align_;cursor_=produced-capacity;
        }
        const auto frames=std::min<std::uint64_t>(bytes/align_,produced-cursor_);
        lhdc::DirectPcmChunk chunk{initial,{}};
        chunk.bytes.resize(static_cast<std::size_t>(frames)*align_);
        for(std::size_t i=0;i<chunk.bytes.size();++i) chunk.bytes[i]=static_cast<std::uint8_t>((cursor_*align_+i)%251);
        cursor_+=frames;
        chunk.state.Running=produced<rate_;
        chunk.state.ProducedBytes=produced*align_;
        chunk.state.DroppedBytes=dropped_;
        chunk.state.BufferedBytes=static_cast<ULONG>((produced-cursor_)*align_);
        chunk.state.PayloadBytes=static_cast<ULONG>(chunk.bytes.size());
        return chunk;
    }
    void reset() { ++resets;cursor_=produced_frames();dropped_=0; }
private:
    std::uint64_t produced_frames() const {
        const auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-started_).count();
        return std::min<std::uint64_t>(rate_,static_cast<std::uint64_t>(ns)*rate_/1000000000);
    }
    unsigned rate_,align_;
    Clock::time_point started_=Clock::now();
    std::uint64_t cursor_=0,dropped_=0;
};
static void wait_preroll(lhdc::PcmReader& reader,unsigned rate,unsigned align) {
    const auto deadline=Clock::now()+std::chrono::seconds(2);
    lhdc::Pacer poll;
    while(reader.stats().queue.queued<rate*align*60/1000) {
        require(Clock::now()<deadline,"Reader preroll timed out");
        poll.wait_until(Clock::now()+std::chrono::milliseconds(1));
    }
}
static void stalled_sender(unsigned rate,unsigned bits,unsigned stall_ms,bool expect_loss) {
    Source source(rate,bits);
    lhdc::PcmReader reader([&](std::size_t bytes){return source.read(bytes);},[&]{source.reset();},source.initial,{rate,bits,400});
    const auto align=2*(bits/8);
    wait_preroll(reader,rate,align);
    std::vector<std::uint8_t> block((rate/100)*align);
    const auto started=Clock::now(),deadline=started+std::chrono::seconds(5);
    lhdc::Pacer pacer;
    std::size_t consumed=0;unsigned packets=0;
    for(;;) {
        require(Clock::now()<deadline,"Stalled sender did not drain or finish");
        if(!reader.pop(block)) {
            if(reader.stats().finished) break;
            pacer.wait_until(Clock::now()+std::chrono::milliseconds(1));continue;
        }
        if(!expect_loss) for(std::size_t i=0;i<block.size();++i) {
            if(block[i]!=static_cast<std::uint8_t>((consumed+i)%251)) {
                const auto stats=reader.stats();
                throw std::runtime_error("PCM mismatch at byte "+std::to_string(consumed+i)+", value="+std::to_string(block[i])
                    +", expected="+std::to_string((consumed+i)%251)+", dropped="+std::to_string(stats.dropped_bytes())
                    +", read_gap_us="+std::to_string(stats.max_read_gap_us));
            }
        }
        consumed+=block.size();
        if(++packets==9) {
            const auto reads=source.reads.load();
            pacer.wait_until(Clock::now()+std::chrono::milliseconds(stall_ms));
            require(source.reads.load()>reads,"PCM reader stopped while sender was blocked");
        }
        pacer.wait_until(started+std::chrono::nanoseconds((consumed/align)*1000000000ull/rate));
    }
    reader.stop();const auto stats=reader.stats();
    require(stats.queue.high_water<=static_cast<std::size_t>(rate)*align*lhdc::pcm_queue_duration_ms/1000,"Send stall grew the queue beyond its duration bound");
    require(stats.queue.queued<block.size(),"End of source left a complete encoder block buffered");
    if(expect_loss) {
        require(stats.kernel_dropped_bytes>0 && stats.discarded_bytes>0 && source.resets>0,"Long stall must report and recover real overflow");
    } else {
        require(consumed==rate*align && !stats.queue.queued && !stats.dropped_bytes() && !source.resets,"Short send stall must preserve the complete source without resets");
    }
    std::cout<<rate<<'/'<<bits<<" stall="<<stall_ms<<"ms high_water="<<stats.queue.high_water
             <<" read_gap_us="<<stats.max_read_gap_us<<" dropped="<<stats.dropped_bytes()<<'\n';
}
int main() {
    try {
        for(const auto rate:{44100u,48000u,96000u,192000u}) for(const auto bits:{16u,24u}) stalled_sender(rate,bits,355,false);
        stalled_sender(48000,24,700,true);
        Source source(48000,24);
        {
            lhdc::PcmReader reader([&](std::size_t)->lhdc::DirectPcmChunk{throw std::runtime_error("source read failed");},[]{},source.initial,{48000,24,400});
            bool propagated=false;const auto deadline=Clock::now()+std::chrono::seconds(1);
            while(Clock::now()<deadline && !propagated) {
                try { reader.stats(); } catch(const std::runtime_error& error) { propagated=std::string(error.what())=="source read failed"; }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            require(propagated,"Reader failure was not delivered to the sender");
        }
        {
            auto changed=source.initial;++changed.Epoch;
            lhdc::PcmReader reader([&](std::size_t){return lhdc::DirectPcmChunk{changed,std::vector<std::uint8_t>(6,1)};},[]{},source.initial,{48000,24,400});
            const auto deadline=Clock::now()+std::chrono::seconds(1);
            while(!reader.stats().finished) { require(Clock::now()<deadline,"Epoch change did not stop the reader");std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            std::vector<std::uint8_t> block(6);
            require(reader.stats().epoch_changed && !reader.pop(block),"Changed format must not supply stale PCM");
        }
        {
            Source full(48000,24);
            lhdc::PcmReader reader([&](std::size_t bytes){return full.read(bytes);},[&]{full.reset();},full.initial,{48000,24,400});
            const auto deadline=Clock::now()+std::chrono::seconds(1);
            while(reader.stats().queue.queued<48000*6*lhdc::pcm_queue_duration_ms/1000) { require(Clock::now()<deadline,"Queue did not reach its bound");std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            const auto stopping=Clock::now();reader.stop();
            require(Clock::now()-stopping<std::chrono::seconds(1),"Stopping a full queue left its producer blocked");
        }
        std::cout<<"Independent PCM capture, overflow accounting, epoch and shutdown checks passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
