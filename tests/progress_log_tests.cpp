#include "service/progress_log.hpp"
#include <future>
#include <sstream>
#include <stdexcept>
#include <iostream>

class SlowDisk:public std::stringbuf {
public:
    std::promise<void> writing;
    std::shared_future<void> release;
    explicit SlowDisk(std::shared_future<void> ready):release(std::move(ready)) {}
    int sync() override {
        if (!blocked_) { blocked_=true; writing.set_value(); release.wait(); }
        return std::stringbuf::sync();
    }
private:
    bool blocked_=false;
};
int main() {
    std::promise<void> resume;
    SlowDisk disk(resume.get_future().share());
    std::ostream output(&disk);
    std::future<void> producer;
    bool nonblocking=false;
    {
        lhdc::ProgressLog log(output);
        log.submit("first");
        disk.writing.get_future().wait();
        producer=std::async(std::launch::async,[&] {
            for (unsigned i=0;i<1000;++i) log.submit(std::to_string(i));
            log.submit("latest");
        });
        nonblocking=producer.wait_for(std::chrono::milliseconds(200))==std::future_status::ready;
        resume.set_value();
        producer.get();
    }
    if (!nonblocking || disk.str()!="first\nlatest\n") {
        std::cerr << "Slow disk blocked the producer or progress snapshots were not coalesced/drained\n";
        return 1;
    }
    std::cout << "Progress logging remains nonblocking with a stalled disk; latest snapshot drains on shutdown\n";
}
