#pragma once
#include "TransferProtocol.h"
#include "QueuedConnection.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace serialctl::xfer {
class TransferController {
  public:
    struct Snapshot {
        std::uint64_t session=0,bytes=0,total=0,expectedReceiveBytes=0;
        bool busy=false,success=false,cancelled=false,peerAcknowledged=false,includesPadding=false;
        bool stopping=false,finishing=false;
        Protocol protocol=Protocol::Ymodem;
        Direction direction=Direction::Send;
        std::wstring source,path,note,partial,peerName;
    };
    struct Request {
        std::uint64_t session=0;
        std::wstring source,path;
        Options options;
        std::shared_ptr<QueuedConnection> connection;
    };
    ~TransferController();
    bool Start(Request,std::wstring& error);
    Snapshot State() const;
    bool Busy() const;
    void Cancel();
    void CancelSession(std::uint64_t);
    void FinishRawReceive();
    void Shutdown(); // Owning cleanup thread, never a window callback.
  private:
    struct Task;
    static void Work(const std::shared_ptr<Task>&);
    mutable std::mutex mutex_;
    std::shared_ptr<Task> task_;
    std::thread worker_;
    inline static std::atomic_uint64_t next_{1};
};
} // namespace serialctl::xfer
