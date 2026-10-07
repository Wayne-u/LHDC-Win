#pragma once
#include "transport/windows.hpp"
#include <windows.h>
#include <lhdc_pcm.h>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <filesystem>
namespace lhdc {
std::optional<std::wstring> find_direct_pcm();
struct DirectPcmChunk { LHDC_PCM_STATE state; std::vector<std::uint8_t> bytes; };
class DirectPcm {
public:
    explicit DirectPcm(const std::wstring& path);
    ~DirectPcm();
    DirectPcm(const DirectPcm&)=delete;
    LHDC_PCM_STATE state();
    DirectPcmChunk read(std::size_t max_bytes=16384);
    void reset();
    void inspect_formats();
private:
    DWORD property(ULONG id,bool set,void* data,DWORD size);
    HANDLE handle_=INVALID_HANDLE_VALUE;
};
void inspect_direct_pcm();
void inspect_direct_formats();
void capture_direct_pcm(unsigned seconds,const std::filesystem::path& output);
}
