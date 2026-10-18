#pragma once
#include <windows.h>
#include <bluetoothapis.h>
#include <cstdint>

namespace lhdc::detail {
struct ConnectionInfo { BLUETOOTH_DEVICE_INFO device{}; bool radio=false,known=false; };
// Keep enumeration and handle lifetime testable without changing real pairings.
struct RadioApi {
    decltype(&BluetoothFindFirstRadio) first=BluetoothFindFirstRadio;
    decltype(&BluetoothFindNextRadio) next=BluetoothFindNextRadio;
    decltype(&BluetoothFindRadioClose) finish=BluetoothFindRadioClose;
    decltype(&BluetoothGetDeviceInfo) device_info=BluetoothGetDeviceInfo;
    decltype(&CloseHandle) close=CloseHandle;
};
ConnectionInfo query_connection(std::uint64_t address,const RadioApi& api={});
}
