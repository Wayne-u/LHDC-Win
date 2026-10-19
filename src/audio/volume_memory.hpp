#pragma once
#include <cmath>
#include <cstdint>
namespace lhdc {
struct VolumeMemory {
    std::uint32_t version=1;
    float scalar=0;
    std::uint32_t muted=0,absolute=0;
};
static_assert(sizeof(VolumeMemory)==16);
inline bool valid_volume_memory(const VolumeMemory& value) {
    return value.version==1 && std::isfinite(value.scalar) && value.scalar>=0 && value.scalar<=1 && value.muted<=1 && value.absolute<=1;
}
inline bool restore_volume_memory(const VolumeMemory& value,bool absolute,bool user_changed) {
    return valid_volume_memory(value) && value.absolute==static_cast<unsigned>(absolute) && !user_changed;
}
}
