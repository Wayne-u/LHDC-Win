#pragma once
#include <cstdint>
#include <span>
#include <vector>
namespace lhdc::avrcp {
using Bytes=std::vector<std::uint8_t>;
constexpr std::uint8_t get_capabilities=0x10,register_notification=0x31,set_absolute_volume=0x50;
constexpr std::uint8_t volume_changed=0x0d;
constexpr std::uint8_t accepted=0x09,stable=0x0c,changed=0x0d,interim=0x0f;
struct Frame {
    std::uint8_t label,ctype,opcode;
    bool response;
    Bytes operands;
};
struct Vendor { std::uint8_t pdu; Bytes parameters; };
Frame parse(std::span<const std::uint8_t> bytes);
Vendor vendor(const Frame& frame);
Bytes command(std::uint8_t label,std::uint8_t ctype,std::uint8_t pdu,std::span<const std::uint8_t> parameters);
Bytes capabilities(std::uint8_t label);
Bytes notification(std::uint8_t label);
Bytes volume(std::uint8_t label,std::uint8_t value);
Bytes reply(const Frame& request,std::uint8_t ctype,std::span<const std::uint8_t> operands);
bool supports_volume(const Frame& frame);
std::uint8_t volume_value(const Frame& frame);
std::uint8_t absolute_volume(float scalar);
}
