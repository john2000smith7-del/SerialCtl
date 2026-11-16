#pragma once
#include <string>
#include <vector>
#include <windows.h>
namespace serialctl {
struct SerialPortInfo {
    std::wstring port, friendlyName;
};
// Common display and selection logic for terminal and power RS232 dialogs.
// The display name is not a valid device path; always open the raw COM port.
inline std::wstring SerialPortDisplayName(const SerialPortInfo &info) {
    std::wstring label = info.friendlyName.empty() ? info.port : info.friendlyName;
    if (!info.port.empty() && label.find(info.port) == std::wstring::npos)
        label += L" (" + info.port + L")";
    return label;
}
inline const SerialPortInfo *SelectedSerialPort(const std::vector<SerialPortInfo> &ports, LRESULT index) {
    return index >= 0 && static_cast<size_t>(index) < ports.size()
        ? &ports[static_cast<size_t>(index)] : nullptr;
}
std::vector<SerialPortInfo> EnumerateSerialPorts();
} // namespace serialctl
