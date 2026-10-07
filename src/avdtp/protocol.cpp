#include "protocol.hpp"
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <cctype>
namespace lhdc::avdtp {
Bytes command(std::uint8_t label, Signal signal, std::span<const std::uint8_t> payload) {
    if (label > 15) throw std::invalid_argument("AVDTP transaction label exceeds 15");
    Bytes result{static_cast<std::uint8_t>(label << 4), static_cast<std::uint8_t>(signal)};
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}
void Reassembler::reset() { pending_.reset(); remaining_ = 0; }
std::optional<Message> Reassembler::consume(std::span<const std::uint8_t> packet) {
    auto fail = [this](const char* text) -> void { reset(); throw std::runtime_error(text); };
    if (packet.empty()) fail("Empty AVDTP packet");
    const auto label = static_cast<std::uint8_t>(packet[0] >> 4);
    const auto type = static_cast<MessageType>(packet[0] & 3);
    const auto fragment = (packet[0] >> 2) & 3;
    if (fragment == 0 || fragment == 1) {
        if (pending_) fail("Interleaved AVDTP messages are unsupported");
        const std::size_t header_size = fragment == 0 ? 2 : 3;
        if (packet.size() < header_size) fail("Truncated AVDTP header");
        const auto raw_signal = packet[header_size - 1];
        if ((raw_signal & 0xc0) != 0 || (raw_signal & 0x3f) == 0) fail("Invalid AVDTP signal identifier");
        Message message{label, type, static_cast<Signal>(raw_signal), Bytes(packet.begin()+header_size, packet.end())};
        if (fragment == 0) return message;
        if (packet[1] < 2) fail("Invalid AVDTP fragment count");
        remaining_ = packet[1] - 1u;
        pending_ = std::move(message);
        return std::nullopt;
    }
    if (!pending_ || label != pending_->label || type != pending_->type) fail("Unexpected AVDTP fragment");
    if ((fragment == 3) != (remaining_ == 1)) fail("AVDTP fragment count mismatch");
    if (pending_->payload.size() + packet.size() - 1 > 65535) fail("AVDTP message exceeds limit");
    pending_->payload.insert(pending_->payload.end(), packet.begin()+1, packet.end());
    --remaining_;
    if (remaining_ != 0) return std::nullopt;
    auto result = std::move(pending_);
    reset();
    return result;
}
std::vector<Endpoint> endpoints(std::span<const std::uint8_t> payload) {
    if (payload.size() % 2) throw std::runtime_error("Truncated Discover endpoint");
    std::vector<Endpoint> result;
    for (std::size_t i=0; i<payload.size(); i+=2) {
        const auto seid = static_cast<std::uint8_t>(payload[i] >> 2);
        if (seid == 0 || seid > 62 || (payload[i]&1) || (payload[i+1]&7))
            throw std::runtime_error("Invalid Discover endpoint");
        for (const auto& existing : result) if (existing.seid == seid) throw std::runtime_error("Duplicate SEID");
        result.push_back({seid, (payload[i]&2)!=0, static_cast<std::uint8_t>(payload[i+1]>>4), (payload[i+1]&8)!=0});
    }
    return result;
}
std::vector<Capability> capabilities(std::span<const std::uint8_t> payload) {
    std::vector<Capability> result;
    while (!payload.empty()) {
        if (payload.size()<2 || payload.size()-2<payload[1]) throw std::runtime_error("Truncated capability TLV");
        const auto length = payload[1];
        if (payload[0] == 0) throw std::runtime_error("Invalid capability category");
        result.push_back({payload[0], Bytes(payload.begin()+2, payload.begin()+2+length)});
        payload = payload.subspan(2+length);
    }
    return result;
}
std::optional<VendorCodec> vendor_codec(const Capability& cap) {
    if (cap.category != 7) return std::nullopt;
    if (cap.data.size()<2) throw std::runtime_error("Truncated media codec");
    if (cap.data[1] != 0xff) return std::nullopt;
    if (cap.data.size()<8 || cap.data[0] != 0) throw std::runtime_error("Invalid vendor audio codec");
    const auto& b = cap.data;
    const auto vendor = static_cast<std::uint32_t>(b[2]) | (static_cast<std::uint32_t>(b[3])<<8) |
        (static_cast<std::uint32_t>(b[4])<<16) | (static_cast<std::uint32_t>(b[5])<<24);
    const auto codec = static_cast<std::uint16_t>(b[6] | (b[7]<<8));
    return VendorCodec{vendor, codec, Bytes(b.begin()+8,b.end())};
}
std::string hex(std::span<const std::uint8_t> bytes) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (auto byte : bytes) out << std::setw(2) << static_cast<unsigned>(byte);
    return out.str();
}
Bytes unhex(const std::string& text) {
    std::string clean;
    for (unsigned char c : text) {
        if (std::isspace(c)) continue;
        if (!std::isxdigit(c)) throw std::invalid_argument("Invalid hex character");
        clean.push_back(static_cast<char>(c));
    }
    if (clean.size()%2) throw std::invalid_argument("Odd hex length");
    Bytes bytes;
    for (std::size_t i=0; i<clean.size(); i+=2)
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(clean.substr(i,2),nullptr,16)));
    return bytes;
}
} // namespace lhdc::avdtp
