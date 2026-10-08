#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>
namespace lhdc::hey_melody {
using Bytes=std::vector<std::uint8_t>;
struct Frame { std::uint16_t command; std::uint8_t sequence; Bytes payload; };
Bytes encode(const Frame& frame);
class Framer {
public:
    void append(std::span<const std::uint8_t> bytes);
    std::optional<Frame> next();
private:
    Bytes pending_;
};
std::optional<bool> hi_res_status(std::span<const std::uint8_t> payload);
using Request=std::function<Bytes(std::uint16_t,std::span<const std::uint8_t>)>;
struct HiResResult { std::optional<bool> before,enabled; bool changed=false; };
HiResResult hi_res(const Request& request,std::optional<bool> desired={});
}
