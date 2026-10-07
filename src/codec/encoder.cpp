#include "encoder.hpp"
#include <stdexcept>
#include <string>
namespace lhdc {
static void check(std::int32_t status, const char* operation) {
    if (status != LHDC_FRET_SUCCESS)
        throw std::runtime_error(std::string(operation)+" failed: "+std::to_string(status));
}
Encoder::Encoder(std::uint32_t payload_mtu) : Encoder(Profile{},payload_mtu) {}
Encoder::Encoder(Profile profile,std::uint32_t payload_mtu) : mtu_(payload_mtu),profile_(profile) {
    const auto quality=quality_index(profile);
    handle_.reset(lhdcv5_enc_new(LHDC_VERSION_1));
    if (!handle_) throw std::runtime_error("Encoder allocation failed");
    check(lhdcv5_enc_init_encoder(handle_.get(),profile.sample_rate,profile.bits,quality,mtu_,20),"encoder init");
    check(lhdcv5_enc_get_block_size(handle_.get(), &block_samples_),"block size");
}
EncodedPacket Encoder::encode(std::span<const std::uint8_t> pcm) {
    if (pcm.size() != block_bytes()) throw std::invalid_argument("PCM must contain exactly one stereo encoder block");
    EncodedPacket packet{std::vector<std::uint8_t>(mtu_),0};
    std::uint32_t written=0;
    check(lhdcv5_enc_encode(handle_.get(),pcm.data(),pcm.size(),packet.payload.data(),packet.payload.size(),&written,&packet.frames),"encode");
    if (written>mtu_ || ((written==0)!=(packet.frames==0))) throw std::runtime_error("Invalid encoder output boundary");
    packet.payload.resize(written);
    return packet;
}
}
