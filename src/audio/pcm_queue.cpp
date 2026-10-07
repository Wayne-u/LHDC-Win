#include "pcm_queue.hpp"
#include <algorithm>
#include <stdexcept>
namespace lhdc {
PcmQueue::PcmQueue(std::size_t capacity,std::size_t frame_bytes):buffer_(capacity),frame_bytes_(frame_bytes) {
    if (!frame_bytes || !capacity || capacity%frame_bytes) throw std::invalid_argument("PCM queue capacity must contain whole frames");
}
void PcmQueue::push(std::span<const std::uint8_t> pcm) {
    if (pcm.size()%frame_bytes_) throw std::invalid_argument("PCM queue input contains a partial frame");
    if (pcm.size()>buffer_.size()-stats_.queued) {
        ++stats_.overruns;
        throw std::runtime_error("PCM queue capacity exceeded");
    }
    const auto write=(read_+stats_.queued)%buffer_.size();
    const auto first=std::min(pcm.size(),buffer_.size()-write);
    std::copy_n(pcm.begin(),first,buffer_.begin()+write);
    std::copy(pcm.begin()+first,pcm.end(),buffer_.begin());
    stats_.queued+=pcm.size(); stats_.pushed+=pcm.size();
    stats_.high_water=std::max(stats_.high_water,stats_.queued);
}
bool PcmQueue::pop(std::span<std::uint8_t> pcm) {
    if (pcm.empty() || pcm.size()%frame_bytes_) throw std::invalid_argument("PCM queue output must contain whole frames");
    if (stats_.queued<pcm.size()) return false;
    const auto first=std::min(pcm.size(),buffer_.size()-read_);
    std::copy_n(buffer_.begin()+read_,first,pcm.begin());
    std::copy_n(buffer_.begin(),pcm.size()-first,pcm.begin()+first);
    read_=(read_+pcm.size())%buffer_.size();
    stats_.queued-=pcm.size(); stats_.popped+=pcm.size();
    return true;
}
}
