#pragma once
#include "windows_audio.hpp"
#include <memory>
namespace lhdc {
class HfpActivity {
public:
    HfpActivity();
    ~HfpActivity();
    bool active() const;
    ComPtr<IMMDevice> capture_device() const;
private:
    bool matches(IMMDevice* device) const;
    GUID container_{};
    struct Sessions;
    std::unique_ptr<Sessions> sessions_;
};
void inspect_hfp_activity();
}
