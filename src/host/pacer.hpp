#pragma once
#include "transport/windows.hpp"
#include <chrono>
namespace lhdc {
class Pacer {
public:
    Pacer();
    ~Pacer();
    Pacer(const Pacer&)=delete;
    Pacer& operator=(const Pacer&)=delete;
    void wait_until(std::chrono::steady_clock::time_point deadline);
private:
    HANDLE timer_;
};
}
