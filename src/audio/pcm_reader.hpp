#pragma once
#include "direct_pcm.hpp"
#include "pcm_converter.hpp"
#include "pcm_queue.hpp"
#include <chrono>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
namespace lhdc {
inline constexpr unsigned pcm_queue_duration_ms=120;
inline constexpr unsigned pcm_preroll_ms=20;
inline constexpr unsigned pcm_recovery_retained_ms=60;
struct PcmReadStats {
    LHDC_PCM_STATE state{};
    QueueStats queue;
    bool finished=false,epoch_changed=false;
    std::uint64_t kernel_dropped_bytes=0,discarded_bytes=0,overrun_recoveries=0,latency_recoveries=0;
    std::uint64_t discarded_frames=0;
    std::int64_t max_read_gap_us=0,max_read_us=0;
    std::chrono::steady_clock::time_point last_input=std::chrono::steady_clock::now();
    std::uint64_t dropped_bytes() const { return kernel_dropped_bytes+discarded_bytes; }
    std::uint64_t recoveries() const { return overrun_recoveries+latency_recoveries; }
};
// One producer owns source reads/resets; the sender only consumes this bounded queue.
class PcmReader {
public:
    using Read=std::function<DirectPcmChunk(std::size_t)>;
    PcmReader(Read read,std::function<void()> reset,const LHDC_PCM_STATE& initial,Profile output);
    ~PcmReader();
    bool pop(std::span<std::uint8_t> pcm,std::uint64_t* discarded_frames=nullptr);
    PcmReadStats stats() const;
    void stop();
private:
    void run(std::stop_token stop);
    Read read_;
    std::function<void()> reset_;
    const LHDC_PCM_STATE initial_;
    const std::size_t frame_bytes_,capacity_,retained_bytes_,read_frames_;
    PcmConverter converter_;
    PcmQueue queue_;
    mutable std::mutex mutex_;
    PcmReadStats stats_;
    std::exception_ptr error_;
    std::jthread worker_;
};
}
