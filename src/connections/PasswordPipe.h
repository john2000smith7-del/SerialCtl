#pragma once
#include <string>
#include <windows.h>

namespace serialctl {
// Plink -pwfile reads a protected, one-use local named pipe. Neither the
// command line nor a disk file contains the password. All APIs exist on Win7.
class PasswordPipe {
  public:
    ~PasswordPipe();
    bool Open(const std::wstring &password, std::wstring &error);
    void Poll();
    const std::wstring &Name() const {
        return name_;
    }

  private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE, event_ = nullptr;
    OVERLAPPED connect_{};
    std::string password_;
    std::wstring name_;
    bool pending_ = false;
};
} // namespace serialctl
