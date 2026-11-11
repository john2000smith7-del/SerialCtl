#include "SerialPorts.h"
#include <iostream>
#include <vector>
using namespace serialctl;
int main() {
    const std::vector<SerialPortInfo> ports{
        {L"COM3", L"USB-SERIAL CH340 (COM3)"},
        {L"COM15", L"USB Serial Device"},
        {L"COM256", L"COM256"},
        {L"COM42", L""},
    };
    if (SerialPortDisplayName(ports[0]) != L"USB-SERIAL CH340 (COM3)" ||
        SerialPortDisplayName(ports[1]) != L"USB Serial Device (COM15)" ||
        SerialPortDisplayName(ports[2]) != L"COM256" ||
        SerialPortDisplayName(ports[3]) != L"COM42") {
        std::cerr << "Shared serial port display formatting failed\n";
        return 1;
    }
    // Long display text must never leak into the physical device path.
    const auto *selected = SelectedSerialPort(ports, 1);
    if (!selected || selected->port != L"COM15" ||
        selected->port == SerialPortDisplayName(*selected)) {
        std::cerr << "Selected power RS232 port must be raw COM name\n";
        return 2;
    }
    if (SelectedSerialPort(ports, -1) ||
        SelectedSerialPort(ports, static_cast<LRESULT>(ports.size())) ||
        SelectedSerialPort({}, 0)) {
        std::cerr << "Invalid serial selection was not rejected\n";
        return 3;
    }
    std::cout << "PASS: shared serial port labels, raw COM mapping and invalid selection\n";
    return 0;
}
