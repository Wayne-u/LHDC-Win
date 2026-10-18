#include "radio.hpp"
#include "windows.hpp"

namespace lhdc::detail {
ConnectionInfo query_connection(std::uint64_t address,const RadioApi& api) {
    ConnectionInfo result;
    result.device.dwSize=sizeof(result.device);
    result.device.Address.ullLong=address;
    BLUETOOTH_FIND_RADIO_PARAMS params{sizeof(params)};
    HANDLE handle=nullptr;
    const auto enumeration=api.first(&params,&handle);
    if (!enumeration) {
        const auto error=GetLastError();
        if (error!=ERROR_NO_MORE_ITEMS) throw WindowsError("Bluetooth radio enumeration",error);
        return result;
    }
    struct Search {
        const RadioApi& api; HBLUETOOTH_RADIO_FIND handle;
        ~Search() { api.finish(handle); }
    } search{api,enumeration};
    result.radio=true;
    for (;;) {
        {
            struct Radio {
                const RadioApi& api; HANDLE handle;
                ~Radio() { api.close(handle); }
            } radio{api,handle};
            BLUETOOTH_DEVICE_INFO info{};
            info.dwSize=sizeof(info); info.Address.ullLong=address;
            const auto status=api.device_info(handle,&info);
            if (status==ERROR_SUCCESS) {
                // A different radio can retain a disconnected pairing of this address.
                if (!result.known || info.fConnected) result.device=info;
                result.known=true;
                if (info.fConnected) return result;
            } else if (status!=ERROR_NOT_FOUND) throw WindowsError("Bluetooth device information",status);
        }
        if (api.next(enumeration,&handle)) continue;
        const auto error=GetLastError();
        if (error!=ERROR_NO_MORE_ITEMS) throw WindowsError("Bluetooth radio enumeration",error);
        return result;
    }
}
}
