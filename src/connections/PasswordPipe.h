#pragma once
#include <string>
#include <windows.h>

namespace serialctl {
// Plink -pwfile reads a protected, one-use local named pipe. Neither the
// command line nor a disk file contains the password. All APIs exist on Win7.
class PasswordPipe {
  public:
    PasswordPipe() = default;
    PasswordPipe(const PasswordPipe &) = delete;
    PasswordPipe &operator=(const PasswordPipe &) = delete;
    ~PasswordPipe();
    void Reset();
    bool Open(const std::wstring &password, std::wstring &error);
    void Poll();
    bool Delivered() const { return password_.empty() && !failed_; }
    bool Failed() const { return failed_; }
    const std::wstring &Name() const {
        return name_;
    }

  private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE, event_ = nullptr;
    OVERLAPPED connect_{};
    std::string password_;
    std::wstring name_;
    bool pending_ = false;
    bool failed_ = false;
};
} // namespace serialctl
