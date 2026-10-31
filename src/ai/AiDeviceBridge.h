#pragma once
#include "BootInterrupt.h"
#include "Connection.h"
#include "TerminalModel.h"
#include "SessionService.h"
#include "PowerService.h"
#include <deque>
#include <memory>
#include <string>
#include <vector>
namespace serialctl {
// UI-thread device policy; owns no HWND, GUI callback or transport thread.
// Receive-side BootInterrupt remains independently synchronized by its class.
class AiDeviceBridge {
public:
    struct State {
        struct AiOutput { std::uint64_t seq; std::wstring timestamp; std::string text; };
        std::deque<AiOutput> aiOutput;
        std::uint64_t aiCursor = 0;
        ULONGLONG aiLastReceivedAt = 0, aiCommandAt = 0;
        ULONGLONG aiObservedAt = 0;
        std::uint64_t aiCommandNumber = 0, aiCommandCursor = 0;
        std::string aiCommandText, recoveryAction, bootPreset;
        std::shared_ptr<BootInterrupt> boot = std::make_shared<BootInterrupt>();
        size_t aiBytes = 0;
        bool aiTruncated = false;
        bool bootRestartUsed = false;
        bool bootPresetLoaded = false;
        unsigned bootIntervalMs = 300;
        std::string aiTransportNotice;
    };
    struct Target {
        State& state;
        TerminalModel& terminal;
        std::uint64_t id;
        int mode;
        const std::wstring& name;
        std::shared_ptr<IConnection> connection;
        unsigned codePage;
        const std::wstring& username;
        const std::wstring& host;
        const std::wstring& remoteCom;
        std::uint16_t port;
        bool udpRaw, udpReceived;
        int lineEndingIndex;
    };
    struct Context {
        std::string instance;
        bool macroRunning = false;
        Json selectedChannels, recoveryBinding;
    };
    AiDeviceBridge(PowerService& power, SessionService& sessions) : power_(power), sessions_(sessions) {}
    Json Request(const std::string&, const Json&, std::vector<Target>&, const Context&,
                 ULONGLONG now, bool& armBootPulse);
    static void Observe(State&, const std::wstring& decoded, const std::wstring& timestamp, ULONGLONG now);
    static bool Pulse(State&, const std::shared_ptr<IConnection>&, ULONGLONG now);
private:
    Json Send(Target&, const Bytes&);
    PowerService& power_;
    SessionService& sessions_;
};
} // namespace serialctl
