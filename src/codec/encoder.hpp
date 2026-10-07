#pragma once
#include <cstdint>
#include <span>
#include <vector>
#include <memory>
#include <lhdcv5_enc.h>
#include "profile.hpp"
namespace lhdc {
struct EncodedPacket { std::vector<std::uint8_t> payload; std::uint32_t frames; };
class Encoder {
public:
    explicit Encoder(std::uint32_t payload_mtu = 660);
    Encoder(Profile profile, std::uint32_t payload_mtu);
    std::uint32_t block_samples() const { return block_samples_; }
    std::size_t block_bytes() const { return block_samples_ * 2u * (profile_.bits/8); }
    EncodedPacket encode(std::span<const std::uint8_t> pcm);
private:
    std::unique_ptr<lhdcv5_enc_t, decltype(&lhdcv5_enc_free)> handle_{nullptr, lhdcv5_enc_free};
    std::uint32_t block_samples_ = 0;
    std::uint32_t mtu_;
    Profile profile_;
};
}
