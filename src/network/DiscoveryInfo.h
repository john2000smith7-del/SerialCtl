#pragma once
#include <mutex>
#include <string>
namespace serialctl
{
inline std::mutex discoveryInfoMutex;
inline std::string discoveryInstance;
inline unsigned discoveryApiPort = 0;
inline std::string ApiDiscoveryReply()
{
    std::lock_guard<std::mutex> lock(discoveryInfoMutex);
    return "SERIALCTL/3 OK\nINSTANCE " + discoveryInstance + "\nAPI " +
           std::to_string(discoveryApiPort) +
           "\nCAPABILITIES serial,cmd,power,http,websocket\nEND\n";
}
} // namespace serialctl
