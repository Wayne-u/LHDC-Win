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
        require(stats.latency_recoveries>0 && stats.discarded_bytes>0,"Long stall must discard stale PCM before the kernel overflows");
        require(!stats.kernel_dropped_bytes && !source.resets,"A blocked sender must not block kernel capture");
        require(consumed+stats.queue.queued+stats.discarded_bytes==rate*align,"Latency recovery must account for every captured byte");
    } else {
        require(consumed==rate*align && !stats.queue.queued && !stats.dropped_bytes() && !source.resets,"Short send stall must preserve the complete source without resets");
    }
    std::cout<<rate<<'/'<<bits<<" stall="<<stall_ms<<"ms high_water="<<stats.queue.high_water
             <<" read_gap_us="<<stats.max_read_gap_us<<" dropped="<<stats.dropped_bytes()<<'\n';
}
int main() {
    try {
        for(const auto rate:{44100u,48000u,96000u,192000u}) for(const auto bits:{16u,24u}) {
            stalled_sender(rate,bits,30,false);
            stalled_sender(rate,bits,355,true);
        }
        stalled_sender(48000,24,700,true);
        {
            Source slow(96000,24);
            lhdc::PcmReader reader([&](std::size_t bytes){return slow.read(bytes);},[&]{slow.reset();},slow.initial,{96000,24,400});
            wait_preroll(reader,96000,6);
            std::vector<std::uint8_t> block(960*6);
            std::size_t consumed=0;
            const auto deadline=Clock::now()+std::chrono::seconds(3);
            while(Clock::now()<deadline) {
                if(reader.pop(block)) consumed+=block.size();
                else if(reader.stats().finished) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            reader.stop();const auto stats=reader.stats();
            require(stats.finished && stats.latency_recoveries>=3,"Sustained slow sends must repeatedly recover latency");
            require(!stats.kernel_dropped_bytes && !slow.resets,"Sustained congestion must keep draining the kernel ring");
            require(stats.queue.high_water<=96000*6*lhdc::pcm_queue_duration_ms/1000,"Sustained congestion must not grow latency without bound");
            require(consumed+stats.queue.queued+stats.discarded_bytes==96000*6,"Sustained congestion must neither duplicate nor silently lose PCM");
        }
        Source source(48000,24);
        {
            unsigned reads=0;
            lhdc::PcmReader reader([&](std::size_t) {
                auto state=source.initial;
                if(++reads<=14) return lhdc::DirectPcmChunk{state,std::vector<std::uint8_t>(2880,1)};
                state.Running=0;return lhdc::DirectPcmChunk{state,{}};
            },[]{},source.initial,{48000,24,500});
            const auto deadline=Clock::now()+std::chrono::seconds(1);
            while(!reader.stats().finished) { require(Clock::now()<deadline,"Dropped frame accounting did not finish");std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            const auto stats=reader.stats();
            require(stats.discarded_bytes==20160 && stats.discarded_frames==3360,"PCM gaps account for removed source frames, not elapsed polling time");
            std::vector<std::uint8_t> block(2880);std::uint64_t discarded=0;
            require(reader.pop(block,&discarded) && discarded==3360,"Each consumed block carries its corresponding PCM discontinuity");
        }
        for(const bool overflow:{false,true}) {
            unsigned reads=0,resets=0;
            lhdc::PcmReader reader([&](std::size_t) {
                auto state=source.initial;
                if(++reads==1) return lhdc::DirectPcmChunk{state,std::vector<std::uint8_t>(2880,1)};
                if(reads==2) {
                    state.BufferedBytes=overflow ? 2880 : 40000;
                    state.DroppedBytes=overflow ? 12 : 0;
                    return lhdc::DirectPcmChunk{state,std::vector<std::uint8_t>(2880,2)};
                }
                state.Running=0;return lhdc::DirectPcmChunk{state,{}};
            },[&]{++resets;},source.initial,{48000,24,400});
            const auto deadline=Clock::now()+std::chrono::seconds(1);
            while(!reader.stats().finished) { require(Clock::now()<deadline,"Kernel backlog recovery did not finish");std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            reader.stop();const auto stats=reader.stats();
            require(resets==1 && stats.recoveries()==1 && !stats.queue.queued,"Kernel backlog recovery must also clear old user PCM");
            require(stats.kernel_dropped_bytes==(overflow?12:0) && stats.discarded_bytes==5760+(overflow?2880:40000),"Kernel backlog and queued PCM losses must both be counted");
            require(stats.discarded_frames==static_cast<std::uint64_t>(5760+(overflow?2892:40000))/6,"Kernel recovery includes skipped raw frames and queued converted frames");
        }
        {
            auto floating=source.initial;floating.Bits=32;floating.BlockAlign=8;floating.FloatingPoint=1;
            unsigned reads=0;
            lhdc::PcmReader reader([&](std::size_t) {
                auto state=floating;
                if(++reads==1) return lhdc::DirectPcmChunk{state,std::vector<std::uint8_t>(3840)};
                if(reads==2) { state.DroppedBytes=8;state.BufferedBytes=7680;return lhdc::DirectPcmChunk{state,std::vector<std::uint8_t>(3840)}; }
                state.Running=0;return lhdc::DirectPcmChunk{state,{}};
            },[]{},floating,{48000,24,500});
            const auto deadline=Clock::now()+std::chrono::seconds(1);
            while(!reader.stats().finished) { require(Clock::now()<deadline,"Float PCM recovery did not finish");std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            require(reader.stats().discarded_frames==1921,"RTP gaps use sample frames when raw float PCM and converted 24-bit PCM have different byte sizes");
        }
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
        std::cout<<"Independent PCM capture, bounded latency, overflow accounting, epoch and shutdown checks passed\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
}
