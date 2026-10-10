#pragma once
#include <memory>
#include <windows.h>

namespace serialctl
{
enum class ToolbarIcon
{
    Ssh,
    Serial,
    Telnet,
    RemoteSerial,
    Cmd,
    Power,
    Moon,
    Sun
};
class ToolbarIcons
{
  public:
    ToolbarIcons();
    ~ToolbarIcons();
    void Draw(HDC dc, ToolbarIcon icon, const RECT &bounds, bool white);

  private:
    struct Images;
    std::unique_ptr<Images> images_;
};
} // namespace serialctl
