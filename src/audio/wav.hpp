#pragma once
#include <cstdint>
#include <vector>
#include <filesystem>
namespace lhdc {
struct Wav { std::uint32_t sample_rate; std::uint32_t bits; std::vector<std::uint8_t> pcm; };
Wav read_wav(const std::filesystem::path& path);
void write_wav(const std::filesystem::path& path,const Wav& wav);
void write_test_wav(const std::filesystem::path& path, unsigned seconds,std::uint32_t sample_rate=48000,std::uint32_t bits=16,bool loop=false,bool silent=false);
}
