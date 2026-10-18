#include "pcm_reader.hpp"
#include "windows_audio.hpp"
#include "host/pacer.hpp"
#include <algorithm>
namespace lhdc {
namespace {
using Clock=std::chrono::steady_clock;
std::int64_t elapsed_us(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count();
}
}
PcmReader::PcmReader(Read read,std::function<void()> reset,const LHDC_PCM_STATE& initial,Profile output)
    :read_(std::move(read)),reset_(std::move(reset)),initial_(initial),
     frame_bytes_(2*(output.bits/8)),
     capacity_(static_cast<std::size_t>(output.sample_rate)*frame_bytes_*pcm_queue_duration_ms/1000),
     retained_bytes_(static_cast<std::size_t>(output.sample_rate)*frame_bytes_*pcm_recovery_retained_ms/1000),
     read_frames_(std::min(16384u/initial.BlockAlign,initial.SampleRate/50)),
     converter_(initial.SampleRate,initial.Bits,initial.FloatingPoint!=0,output),queue_(capacity_,frame_bytes_) {
    stats_.state=initial;
    worker_=std::jthread([this](std::stop_token stop){run(stop);});
}
PcmReader::~PcmReader() { stop(); }
void PcmReader::stop() {
    if(!worker_.joinable()) return;
    { std::lock_guard lock(mutex_);worker_.request_stop(); }
    worker_.join();
}
bool PcmReader::pop(std::span<std::uint8_t> pcm,std::uint64_t* discarded_frames) {
    std::lock_guard lock(mutex_);
    if(error_) std::rethrow_exception(error_);
    if(stats_.epoch_changed) return false;
    if(!queue_.pop(pcm)) return false;
    if(discarded_frames) *discarded_frames=stats_.discarded_frames;
    return true;
}
PcmReadStats PcmReader::stats() const {
    std::lock_guard lock(mutex_);
    if(error_) std::rethrow_exception(error_);
    auto result=stats_;result.queue=queue_.stats();return result;
}
void PcmReader::run(std::stop_token stop) {
    try {
        AudioTask priority;
        Pacer poll;
        auto last_read=Clock::now();
        while(!stop.stop_requested()) {
            const auto started=Clock::now();
            const auto gap_us=elapsed_us(last_read);last_read=started;
            auto chunk=read_(read_frames_*initial_.BlockAlign);
            const auto read_us=elapsed_us(started);
            const bool changed=chunk.state.Epoch!=initial_.Epoch;
            const bool overrun=!changed && chunk.state.DroppedBytes!=0;
            const auto kernel_limit=static_cast<std::uint64_t>(initial_.SampleRate)*initial_.BlockAlign*pcm_queue_duration_ms/1000;
            const bool stale=!changed && chunk.bytes.size()+chunk.state.BufferedBytes>kernel_limit;
            if(overrun || stale) reset_();
            std::vector<std::uint8_t> converted;
            if(!changed && !overrun && !stale && !chunk.bytes.empty()) converted=converter_.convert(chunk.bytes);
            std::unique_lock lock(mutex_);
            stats_.state=chunk.state;
            stats_.max_read_gap_us=std::max(stats_.max_read_gap_us,gap_us);
            stats_.max_read_us=std::max(stats_.max_read_us,read_us);
            if(changed) { stats_.epoch_changed=true;stats_.finished=true;return; }
            if(overrun || stale) {
                const auto queued_discarded=queue_.trim(0);
                stats_.kernel_dropped_bytes+=chunk.state.DroppedBytes;
                stats_.discarded_bytes+=chunk.bytes.size()+chunk.state.BufferedBytes+queued_discarded;
                stats_.discarded_frames+=(chunk.bytes.size()+chunk.state.BufferedBytes+chunk.state.DroppedBytes)/initial_.BlockAlign+queued_discarded/frame_bytes_;
                if(overrun) ++stats_.overrun_recoveries;
                else ++stats_.latency_recoveries;
                stats_.last_input=Clock::now();
            } else if(!converted.empty()) {
                // Keep reading while Bluetooth is blocked. Otherwise the
                // kernel ring becomes a second FIFO of increasingly old PCM.
                if(queue_.stats().queued+converted.size()>capacity_) {
                    const auto discarded=queue_.trim(retained_bytes_-converted.size());
                    stats_.discarded_bytes+=discarded;
                    stats_.discarded_frames+=discarded/frame_bytes_;
                    ++stats_.latency_recoveries;
                }
                queue_.push(converted);stats_.last_input=Clock::now();
            } else if(!chunk.state.Running) { stats_.finished=true;return; }
            if(converted.empty()) {
                lock.unlock();
                poll.wait_until(Clock::now()+std::chrono::milliseconds(2));
            }
        }
    } catch(...) {
        std::lock_guard lock(mutex_);error_=std::current_exception();stats_.finished=true;
    }
}
}
