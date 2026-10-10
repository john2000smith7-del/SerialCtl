#pragma once
#include "Connection.h"
#include <atomic>
#include <mutex>
#include <thread>
namespace serialctl
{
class CmdConnection final : public IConnection
{
  public:
    ~CmdConnection() override
    {
        Stop();
    }
    bool Start(DataCallback data, StatusCallback status, std::wstring &error) override;
    void Stop() override;
    bool Send(const Bytes &bytes, std::wstring &error) override;
    bool IsConnected() const override
    {
        return connected_;
    }

  private:
    void Read();
    HANDLE process_ = nullptr, job_ = nullptr, input_ = nullptr, output_ = nullptr;
    std::atomic_bool connected_{false}, stopping_{false};
    std::thread reader_;
    std::mutex write_;
    DataCallback data_;
    StatusCallback status_;
};
} // namespace serialctl
