#pragma once
#include <cstdint>
#include <vector>
#include <span>
namespace lhdc {
struct QueueStats {
    std::uint64_t pushed=0,popped=0,overruns=0;
    std::size_t queued=0,high_water=0;
};
// Caller provides synchronization when producer and consumer use separate threads.
class PcmQueue {
public:
    PcmQueue(std::size_t capacity,std::size_t frame_bytes);
    void push(std::span<const std::uint8_t> pcm);
    bool pop(std::span<std::uint8_t> pcm);
    QueueStats stats() const { return stats_; }
private:
    std::vector<std::uint8_t> buffer_;
    std::size_t frame_bytes_,read_=0;
    QueueStats stats_;
};
}
