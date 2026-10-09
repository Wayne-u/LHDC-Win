#include "bitrate_control.hpp"
#include <algorithm>
namespace lhdc {
namespace {
constexpr std::uint32_t floor_kbps=400,initial_limit_kbps=500;
constexpr std::uint64_t sample_interval_ms=100,stable_interval_ms=15000,recovery_interval_ms=30000;
constexpr double stable_queue_ms=80,pressure_queue_ms=120,urgent_queue_ms=180;
constexpr double pressure_pending_ms=40,urgent_pending_ms=100;
constexpr unsigned pressure_sample_count=3;
}
BitrateControl::BitrateControl(Profile ceiling,bool adaptive,std::uint64_t now_ms)
    :last_sample_(now_ms),stable_since_(now_ms),next_raise_(now_ms+stable_interval_ms),adaptive_(adaptive) {
    quality_index(ceiling);
    const auto floor=std::min(floor_kbps,ceiling.kbps);
    for (const auto kbps:bitrates(ceiling.sample_rate))
        if (kbps>=floor && kbps<=ceiling.kbps) levels_.push_back(kbps);
    index_=levels_.size()-1;
    if (adaptive_)
        while (levels_[index_]>initial_limit_kbps) --index_;
}
std::uint32_t BitrateControl::observe(std::uint64_t now_ms,double queue_ms,std::uint64_t dropped_bytes,double pending_ms) {
    if (!adaptive_) return bitrate();
    max_pending_ms_=std::max(max_pending_ms_,pending_ms);
    if (now_ms-last_sample_<sample_interval_ms) return bitrate();
    last_sample_=now_ms;
    pending_ms=max_pending_ms_;
    max_pending_ms_=0;
    const bool dropped=dropped_bytes>last_dropped_;
    last_dropped_=dropped_bytes;
    // A 60ms preroll is normal. React before the bounded PCM queues overflow.
    const bool sending_backlog=pending_ms>=pressure_pending_ms && queue_ms>stable_queue_ms;
    pressure_samples_=queue_ms>=pressure_queue_ms || sending_backlog ? pressure_samples_+1 : 0;
    const bool urgent=dropped || queue_ms>=urgent_queue_ms || (pending_ms>=urgent_pending_ms && queue_ms>stable_queue_ms);
    if (urgent || pressure_samples_>=pressure_sample_count) {
        if (urgent && bitrate()>initial_limit_kbps) {
            while (bitrate()>initial_limit_kbps) --index_;
        } else if (index_>0) --index_;
        pressure_samples_=0;
        stable_since_=now_ms;
        next_raise_=now_ms+recovery_interval_ms;
    } else if (queue_ms>stable_queue_ms) {
        stable_since_=now_ms;
    } else if (now_ms-stable_since_>=stable_interval_ms && now_ms>=next_raise_ && index_+1<levels_.size()) {
        ++index_;
        stable_since_=now_ms;
        next_raise_=now_ms+stable_interval_ms;
    }
    return bitrate();
}
}
