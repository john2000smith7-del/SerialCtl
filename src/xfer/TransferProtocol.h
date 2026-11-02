#pragma once
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace serialctl::xfer {
using Bytes=std::vector<std::uint8_t>;
enum class Protocol { Raw, Xmodem, Ymodem, Zmodem };
enum class Direction { Send, Receive };
struct Options {
    Protocol protocol=Protocol::Ymodem;
    Direction direction=Direction::Send;
    std::string filename;
    std::uint64_t size=0;
    std::uint64_t receiveLimit=4ull*1024*1024*1024;
    std::uint64_t expectedReceiveBytes=0;
    unsigned timeoutMs=10000, retries=10;
};
struct Io {
    // Return -1 on bounded timeout; other channel/file failures throw.
    std::function<int(unsigned)> readByte;
    std::function<void(const Bytes&)> write;
    std::function<std::size_t(std::uint64_t,std::uint8_t*,std::size_t)> readFile;
    std::function<void(const std::uint8_t*,std::size_t)> writeFile;
    std::function<void(const std::string&,std::uint64_t,bool)> metadata;
    std::function<void(std::uint64_t,std::uint64_t)> progress;
    std::function<bool()> cancelled,finishRaw;
    std::function<void()> abortWire;
};
struct Result {
    std::uint64_t bytes=0;
    bool peerAcknowledged=false, includesPadding=false;
    std::string filename;
};
class Cancelled : public std::runtime_error {
  public: Cancelled():std::runtime_error("Transfer cancelled; already sent bytes cannot be recalled"){}
};
std::uint16_t Crc16(const std::uint8_t*,std::size_t);
std::uint32_t Crc32(const std::uint8_t*,std::size_t);
Result Run(const Options&,const Io&);
} // namespace serialctl::xfer
