#pragma once
#include <cstdint>
namespace serialctl {
struct CmdOutputHeader { std::uint32_t size, inputCodePage, outputCodePage; };
// Internal mode of this same native executable, launched only with private pipes.
int RunCmdConsoleBridge();
} // namespace serialctl
