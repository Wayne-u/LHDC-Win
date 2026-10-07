#pragma once
#include "transport/windows.hpp"
#include <atomic>
#include <exception>
#include <thread>
namespace lhdc {
// Only this worker accesses AVRCP. Bluetooth control never blocks PCM consumption.
class VolumeSync {
public:
    explicit VolumeSync(Transport& transport);
    ~VolumeSync();
    VolumeSync(const VolumeSync&)=delete;
    void check() const;
private:
    std::exception_ptr error_;
    std::atomic<bool> failed_=false;
    std::jthread worker_;
};
}
