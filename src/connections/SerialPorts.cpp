#include "SerialPorts.h"
#include <algorithm>
#include <setupapi.h>

namespace serialctl {
std::vector<SerialPortInfo> EnumerateSerialPorts() {
    std::vector<SerialPortInfo> result;
    const GUID ports = {0x4d36e978, 0xe325, 0x11ce, {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    const auto devices = SetupDiGetClassDevsW(&ports, nullptr, nullptr, DIGCF_PRESENT);
    if (devices != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA info{sizeof(info)};
        for (DWORD index = 0; SetupDiEnumDeviceInfo(devices, index, &info); ++index) {
            const HKEY key = SetupDiOpenDevRegKey(devices, &info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_QUERY_VALUE);
            wchar_t port[256]{};
            DWORD size = sizeof(port), type = 0;
            const bool found = key != INVALID_HANDLE_VALUE &&
                               RegQueryValueExW(key, L"PortName", nullptr, &type, reinterpret_cast<BYTE *>(port),
                                                &size) == ERROR_SUCCESS &&
                               type == REG_SZ;
            if (key != INVALID_HANDLE_VALUE)
                RegCloseKey(key);
            port[255] = 0;
            if (!found || wcsncmp(port, L"COM", 3) != 0)
                continue;
            wchar_t friendly[1024]{};
            if (!SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_FRIENDLYNAME, nullptr,
                                                   reinterpret_cast<BYTE *>(friendly), sizeof(friendly), nullptr))
                SetupDiGetDeviceRegistryPropertyW(devices, &info, SPDRP_DEVICEDESC, nullptr,
                                                  reinterpret_cast<BYTE *>(friendly), sizeof(friendly), nullptr);
            friendly[1023] = 0;
            result.push_back({port, *friendly ? friendly : port});
        }
        SetupDiDestroyDeviceInfoList(devices);
    }
    // Some legacy virtual-port providers do not populate the Ports class.
    for (int i = 1; i <= 256; ++i) {
        auto name = L"COM" + std::to_wstring(i);
        wchar_t target[256]{};
        if (QueryDosDeviceW(name.c_str(), target, 256) &&
            std::none_of(result.begin(), result.end(), [&](const SerialPortInfo &p) { return p.port == name; }))
            result.push_back({name, name});
    }
    std::sort(result.begin(), result.end(), [](const SerialPortInfo &a, const SerialPortInfo &b) {
        return wcstoul(a.port.c_str() + 3, nullptr, 10) < wcstoul(b.port.c_str() + 3, nullptr, 10);
    });
    return result;
}
} // namespace serialctl
