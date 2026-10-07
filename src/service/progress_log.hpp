#pragma once
#include <condition_variable>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <syncstream>
#include <thread>

namespace lhdc {
// Progress is a snapshot. Keep only the latest pending snapshot if disk I/O
// stalls, so diagnostics cannot block PCM consumption or grow without bound.
class ProgressLog {
public:
    explicit ProgressLog(std::ostream& output):worker_([this,&output] {
        for (;;) {
            std::unique_lock lock(mutex_);
            changed_.wait(lock,[this] { return stopping_ || pending_.has_value(); });
            if (!pending_) return;
            auto line=std::move(*pending_);
            pending_.reset();
            lock.unlock();
            std::osyncstream(output) << line << std::endl;
        }
    }) {}
    ~ProgressLog() { finish(); }
    void finish() {
        if (!worker_.joinable()) return;
        { std::lock_guard lock(mutex_); stopping_=true; }
        changed_.notify_one();
        worker_.join();
    }
    void submit(std::string line) {
        { std::lock_guard lock(mutex_); pending_=std::move(line); }
        changed_.notify_one();
    }
private:
    std::mutex mutex_;
    std::condition_variable changed_;
    std::optional<std::string> pending_;
    bool stopping_=false;
    std::thread worker_;
};
}
