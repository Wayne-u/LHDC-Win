#pragma once
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <optional>
#include <span>
#include <string>
#include <string_view>
namespace lhdc {
struct TargetDevice {
    std::wstring instance;
    std::uint64_t address=0;
    std::wstring name,container,service;
    bool connected=false,hey_melody=false;
};
inline bool same_instance(std::wstring_view a,std::wstring_view b) {
    return a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](wchar_t x,wchar_t y){return std::towupper(x)==std::towupper(y);});
}
inline std::optional<std::uint64_t> audio_sink_address(std::wstring_view instance) {
    constexpr std::wstring_view prefix=L"BTHENUM\\{0000110B-0000-1000-8000-00805F9B34FB}";
    if (instance.size()<=prefix.size() || !same_instance(instance.substr(0,prefix.size()),prefix) ||
        (instance[prefix.size()]!=L'_' && instance[prefix.size()]!=L'\\')) return {};
    const auto suffix=instance.rfind(L'_');
    const auto separator=instance.rfind(L'\\');
    if (suffix==std::wstring_view::npos || suffix<13 || suffix-13<=separator || instance[suffix-13]!=L'&') return {};
    std::uint64_t address=0;
    for (const auto c:instance.substr(suffix-12,12)) {
        const auto digit=std::towupper(c);
        if (!((digit>=L'0' && digit<=L'9') || (digit>=L'A' && digit<=L'F'))) return {};
        address=(address<<4)|static_cast<unsigned>(digit<=L'9'?digit-L'0':digit-L'A'+10);
    }
    return address?std::optional{address}:std::nullopt;
}
inline std::optional<std::size_t> target_index(std::span<const TargetDevice> devices,std::wstring_view selected) {
    if (!selected.empty()) {
        for (std::size_t i=0;i<devices.size();++i) if (same_instance(devices[i].instance,selected)) return i;
        return {};
    }
    std::optional<std::size_t> bound;
    for (std::size_t i=0;i<devices.size();++i) if (same_instance(devices[i].service,L"lhdc-transport")) {
        if (bound) return {};
        bound=i;
    }
    if (bound) return bound;
    return devices.size()==1?std::optional<std::size_t>{0}:std::nullopt;
}
}
