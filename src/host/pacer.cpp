#include "pacer.hpp"
namespace lhdc {
Pacer::Pacer():timer_(CreateWaitableTimerExW(nullptr,nullptr,CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,TIMER_MODIFY_STATE|SYNCHRONIZE)) {
    if (!timer_) throw WindowsError("High-resolution timer creation",GetLastError());
}
Pacer::~Pacer() { CloseHandle(timer_); }
void Pacer::wait_until(std::chrono::steady_clock::time_point deadline) {
    const auto remaining=std::chrono::duration_cast<std::chrono::nanoseconds>(deadline-std::chrono::steady_clock::now()).count();
    if (remaining<=0) return;
    LARGE_INTEGER due{};
    due.QuadPart=-((remaining+99)/100); // relative 100ns, round upward
    if (!SetWaitableTimer(timer_,&due,0,nullptr,nullptr,FALSE)) throw WindowsError("Media timer arm",GetLastError());
    if (WaitForSingleObject(timer_,INFINITE)!=WAIT_OBJECT_0) throw WindowsError("Media timer wait",GetLastError());
}
}
