#pragma once
#include "profile.hpp"
#include <cstdint>
#include <vector>
namespace lhdc {
// The caller applies changes only after the encoder has emitted a whole packet.
class BitrateControl {
public:
    BitrateControl(Profile ceiling,bool adaptive,std::uint64_t now_ms=0);
    std::uint32_t bitrate() const { return levels_[index_]; }
    std::uint32_t observe(std::uint64_t now_ms,double queue_ms,std::uint64_t dropped_bytes,double pending_ms);
private:
    std::vector<std::uint32_t> levels_;
    std::size_t index_=0;
    std::uint64_t last_sample_,stable_since_,next_raise_,last_dropped_=0;
    unsigned pressure_samples_=0;
    double max_pending_ms_=0;
    bool adaptive_;
};
}
