#pragma once
#include <string>
#include <vector>
#include <windows.h>
namespace serialctl {
struct SerialPortInfo {
    std::wstring port, friendlyName;
};
std::vector<SerialPortInfo> EnumerateSerialPorts();
} // namespace serialctl
