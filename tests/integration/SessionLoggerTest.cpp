#include "SessionLogger.h"
#include <iostream>
#include <thread>
using namespace serialctl;
int main()
{
    SessionLogger logger;
    std::wstring error;
    if (!logger.Start(L"rotation-test", error, true))
        return 1;
    const std::wstring path = logger.Path(), copy = path + L".export";
    Bytes block(65536, 'A');
    for (int i = 0; i < 65 * 16; ++i)
    {
        logger.WriteRaw(block);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!logger.SaveCopy(copy, error) || !logger.Error().empty())
        return 2;
    HANDLE file = CreateFileW(copy.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              0, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return 3;
    LARGE_INTEGER length{};
    bool valid = GetFileSizeEx(file, &length) && length.QuadPart == 65ll * 1024 * 1024;
    Bytes buffer(65536);
    DWORD count = 0;
    while (valid &&
           ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) &&
           count)
        for (DWORD i = 0; i < count; ++i)
            if (buffer[i] != 'A')
                valid = false;
    CloseHandle(file);
    // Saving over any active source must leave its content intact.
    valid = valid && !logger.SaveCopy(path, error);
    logger.Stop();
    const auto dot = path.find_last_of(L'.');
    std::wstring second = path.substr(0, dot) + L".part2" + path.substr(dot);
    valid = valid && GetFileAttributesW(second.c_str()) != INVALID_FILE_ATTRIBUTES;
    DeleteFileW(path.c_str());
    DeleteFileW(second.c_str());
    DeleteFileW(copy.c_str());
    if (!valid)
        return 4;
    std::cout << "65 MiB logging, rotation, complete export and source protection passed\n";
    return 0;
}
