#pragma once
#include <windows.h>
#include <winioctl.h>
#include <guiddef.h>
#include <lhdc_transport.h>
#include <string>
#include <vector>
#include <span>
#include <cstdint>
#include <array>
#include <optional>
#include <stdexcept>
#include <memory>
#include <chrono>
namespace lhdc {
struct TargetDevice { std::wstring instance; std::uint64_t address; };
std::optional<TargetDevice> discover_target();
const TargetDevice& target_device();
void inspect_target();
static_assert(sizeof(LHDC_INFO)==80 && sizeof(LHDC_SDU_HEADER)==16 && sizeof(LHDC_OPEN_INPUT)==24 && sizeof(LHDC_CHANNEL_INPUT)==16);
class WindowsError : public std::runtime_error {
public:
    WindowsError(const char* operation,DWORD error):std::runtime_error(std::string(operation)+" failed, Win32="+std::to_string(error)),code(error){}
    DWORD code;
};
void inspect_windows();
void inspect_connection(std::uint64_t address);
bool bluetooth_connected(std::uint64_t address);
struct SendTiming {
    std::int64_t submit_us=0,wait_us=0,completion_us=0,pending_age_us=0;
    bool pending=false;
};
class Transport {
public:
    class PendingSend {
    public:
        ~PendingSend();
        PendingSend(const PendingSend&)=delete;
        PendingSend& operator=(const PendingSend&)=delete;
        SendTiming wait();
        std::int64_t pending_age_us() const;
        const SendTiming& timing() const { return timing_; }
    private:
        friend class Transport;
        PendingSend(HANDLE handle,std::vector<std::uint8_t> input);
        HANDLE handle_;
        std::vector<std::uint8_t> input_;
        OVERLAPPED overlapped_{};
        SendTiming timing_;
        std::chrono::steady_clock::time_point submitted_;
        bool active_=false;
    };
    explicit Transport(std::uint64_t address);
    ~Transport();
    Transport(const Transport&)=delete;
    Transport& operator=(const Transport&)=delete;
    LHDC_INFO info(ULONG channel=LHDC_CHANNEL_SIGNAL);
    void open(ULONG channel=LHDC_CHANNEL_SIGNAL);
    void close(ULONG channel=LHDC_CHANNEL_SIGNAL);
    SendTiming send(std::span<const std::uint8_t> bytes,ULONG channel=LHDC_CHANNEL_SIGNAL);
    std::unique_ptr<PendingSend> begin_send(std::span<const std::uint8_t> bytes,ULONG channel);
    std::vector<std::uint8_t> receive(ULONG channel=LHDC_CHANNEL_SIGNAL,DWORD timeout_ms=10000);
    void cancel_pending();
private:
    DWORD ioctl(DWORD code,void* input,DWORD input_size,void* output,DWORD output_size,DWORD timeout_ms=10000);
    HANDLE handle_=INVALID_HANDLE_VALUE;
    std::uint64_t address_;
    std::array<bool,LHDC_CHANNEL_COUNT> connected_{};
    std::array<ULONG,LHDC_CHANNEL_COUNT> in_mtu_{},out_mtu_{};
};
}
