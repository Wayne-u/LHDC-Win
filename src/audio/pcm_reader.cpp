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
     capacity_(static_cast<std::size_t>(output.sample_rate)*2*(output.bits/8)*pcm_queue_duration_ms/1000),output_align_(2*(output.bits/8)),
     read_frames_(std::min(16384u/initial.BlockAlign,initial.SampleRate/50)),
     converter_(initial.SampleRate,initial.Bits,initial.FloatingPoint!=0,output),queue_(capacity_,output_align_) {
    stats_.state=initial;
    worker_=std::jthread([this](std::stop_token stop){run(stop);});
}
PcmReader::~PcmReader() { stop(); }
void PcmReader::stop() {
    if(!worker_.joinable()) return;
    { std::lock_guard lock(mutex_);worker_.request_stop(); }
    changed_.notify_one();worker_.join();
}
bool PcmReader::pop(std::span<std::uint8_t> pcm) {
    std::lock_guard lock(mutex_);
    if(error_) std::rethrow_exception(error_);
    if(stats_.epoch_changed) return false;
    const auto ready=queue_.pop(pcm);
    if(ready) changed_.notify_one();
    return ready;
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
            std::size_t frames=0;
            {
                std::unique_lock lock(mutex_);
                if(capacity_-queue_.stats().queued<output_align_) {
                    const auto waiting=Clock::now();
                    changed_.wait(lock,[&]{return stop.stop_requested() || capacity_-queue_.stats().queued>=output_align_;});
                    stats_.max_queue_wait_us=std::max(stats_.max_queue_wait_us,elapsed_us(waiting));
                }
                if(stop.stop_requested()) return;
                frames=std::min(read_frames_,(capacity_-queue_.stats().queued)/output_align_);
            }
            const auto started=Clock::now();
            const auto gap_us=elapsed_us(last_read);last_read=started;
            auto chunk=read_(frames*initial_.BlockAlign);
            const auto read_us=elapsed_us(started);
            const bool changed=chunk.state.Epoch!=initial_.Epoch;
            const bool overrun=!changed && chunk.state.DroppedBytes!=0;
            if(overrun) reset_();
            std::vector<std::uint8_t> converted;
            if(!changed && !overrun && !chunk.bytes.empty()) converted=converter_.convert(chunk.bytes);
            std::unique_lock lock(mutex_);
            stats_.state=chunk.state;
            stats_.max_read_gap_us=std::max(stats_.max_read_gap_us,gap_us);
            stats_.max_read_us=std::max(stats_.max_read_us,read_us);
            if(changed) { stats_.epoch_changed=true;stats_.finished=true;return; }
            if(overrun) {
                stats_.kernel_dropped_bytes+=chunk.state.DroppedBytes;
                stats_.discarded_bytes+=chunk.bytes.size()+chunk.state.BufferedBytes;
                ++stats_.overrun_recoveries;stats_.last_input=Clock::now();
            } else if(!converted.empty()) {
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
