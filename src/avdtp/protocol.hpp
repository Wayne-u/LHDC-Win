#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include <optional>
#include <string>
namespace lhdc::avdtp {
using Bytes = std::vector<std::uint8_t>;
enum class Signal : std::uint8_t { discover=1, get_capabilities=2, set_configuration=3,
    get_configuration=4, open=6, start=7, close=8, suspend=9, abort=10, get_all_capabilities=12 };
enum class MessageType : std::uint8_t { command=0, general_reject=1, accept=2, reject=3 };
struct Message { std::uint8_t label; MessageType type; Signal signal; Bytes payload; };
struct Endpoint { std::uint8_t seid; bool in_use; std::uint8_t media_type; bool sink; };
struct Capability { std::uint8_t category; Bytes data; };
struct VendorCodec { std::uint32_t vendor_id; std::uint16_t codec_id; Bytes data; };
Bytes command(std::uint8_t label, Signal signal, std::span<const std::uint8_t> payload = {});
class Reassembler {
public:
    std::optional<Message> consume(std::span<const std::uint8_t> packet);
    void reset();
private:
    std::optional<Message> pending_;
    unsigned remaining_ = 0;
};
std::vector<Endpoint> endpoints(std::span<const std::uint8_t> payload);
std::vector<Capability> capabilities(std::span<const std::uint8_t> payload);
std::optional<VendorCodec> vendor_codec(const Capability& capability);
std::string hex(std::span<const std::uint8_t> bytes);
Bytes unhex(const std::string& text);
} // namespace lhdc::avdtp
