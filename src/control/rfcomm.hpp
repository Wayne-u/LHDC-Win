#pragma once
#include <optional>
#include <cstdint>
namespace lhdc {
void inspect_hi_res(std::optional<bool> desired={});
std::uint32_t capability_revision();
}
