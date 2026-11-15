#pragma once
#include "ByteTypes.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace serialctl {

// The power worker owns the physical cycle boundaries. Only an explicitly
// configured local cycle attaches an observer.
class PowerCycleObserver {
  public:
    enum class CycleAction { Continue, PowerOffContinue, Pause, StopPowerOff, StopKeepPower };
    virtual ~PowerCycleObserver() = default;
    virtual bool BeginRound() = 0;
    virtual bool EndRound() = 0; // True when all actions for this round have finished writing.
    virtual bool Healthy() = 0;
    virtual void Finish() = 0;
    virtual void Cancel(const wchar_t *reason) = 0;
    virtual CycleAction PollAction() { return CycleAction::Continue; }
    virtual bool Resume() { return true; }
};

enum class PowerKeywordAction {
    None = 0,
    RunCommands = 1,
    StopTest = 2,
    StopTestPowerOff = 3,
    PowerOffContinue = 4,
    RunCommandsStopTest = 5,
    PauseAll = 6,
};

enum class PowerKeywordCommandTrigger {
    OnHit = 0,
    OnMiss = 1,
    Both = 2,
};

const wchar_t *PowerKeywordActionLabel(PowerKeywordAction action);
const char *PowerKeywordActionKey(PowerKeywordAction action);
PowerKeywordAction ParsePowerKeywordAction(const std::string &key);

struct PowerKeywordCommand {
    std::wstring text;
    unsigned delayMs = 0;
    unsigned intervalMs = 0;
    PowerKeywordCommandTrigger trigger = PowerKeywordCommandTrigger::OnHit;
};

struct PowerKeywordRule {
    std::wstring keyword, onHit, onMiss, begin, end;
    unsigned hitDelayMs = 0, missDelayMs = 0;
    PowerKeywordAction hitAction = PowerKeywordAction::None;
    PowerKeywordAction missAction = PowerKeywordAction::None;
    std::vector<PowerKeywordCommand> commands{};
};

struct PowerKeywordConfig {
    std::wstring ready;
    std::vector<PowerKeywordRule> rules;
};

class PowerKeywordRun final : public PowerCycleObserver, public std::enable_shared_from_this<PowerKeywordRun> {
  public:
    struct Command {
        Bytes bytes;
        std::wstring display;
        unsigned delayMs = 0;
        unsigned intervalMs = 0;
    };
    struct Rule {
        PowerKeywordRule display;
        std::string keyword, begin, end;
        Bytes onHit, onMiss;
        PowerKeywordAction hitAction = PowerKeywordAction::RunCommands;
        PowerKeywordAction missAction = PowerKeywordAction::RunCommands;
        std::vector<Command> hitCommands{};
        std::vector<Command> missCommands{};
    };
    enum class Window { Waiting, Open, Closed, Interrupted };
    enum class State { Pending, Listening, Draining, Paused, BetweenRounds, Finished, Canceled, Failed };
    struct Count {
        std::uint64_t round = 0, total = 0, missRound = 0, missTotal = 0, written = 0, failed = 0, canceled = 0;
        Window window = Window::Waiting;
    };
    struct Snapshot {
        State state = State::Pending;
        unsigned round = 0;
        bool ready = false, readyExpected = false, timedOut = false;
        std::wstring target, reason;
        std::vector<Count> counts;
    };
    enum class Delivery { Written, NotWritten, Unknown };
    using Completion = std::function<void(Delivery, const std::wstring &)>;
    using Sender = std::function<bool(const Bytes &, const std::function<bool()> &, Completion, std::wstring &)>;
    using Audit = std::function<void(const std::wstring &)>;

    PowerKeywordRun(std::uint64_t session, std::wstring target, std::string ready, std::vector<Rule> rules,
                    Sender send, std::function<bool()> connected, Audit audit);
    ~PowerKeywordRun() override;
    std::uint64_t Session() const { return session_; }
    bool Active() const { return permit_->load(); }
    void Feed(const Bytes &bytes);
    bool BeginRound() override;
    bool EndRound() override;
    bool Healthy() override;
    void Finish() override;
    void Cancel(const wchar_t *reason) override;
    CycleAction PollAction() override;
    bool Resume() override;
    Snapshot Status() const;

  private:
    struct Match {
        explicit Match(std::string pattern);
        bool Feed(unsigned char byte);
        std::string pattern;
        std::vector<size_t> prefix;
        size_t matched = 0;
    };
    struct RuleState {
        explicit RuleState(Rule rule);
        Rule rule;
        Match keyword, begin, end;
        Count count;
    };
    using Clock = std::chrono::steady_clock;
    struct Action {
        size_t index = 0;
        unsigned round = 0;
        bool missing = false;
        bool hasCommand = false;
        Bytes bytes;
        std::wstring commandDisplay;
        PowerKeywordAction followUp = PowerKeywordAction::None;
        Clock::time_point due{};
        std::uint64_t sequence = 0;
    };
    void Hit(size_t index);
    void Close(size_t index);
    void Queue(size_t index, bool missing);
    void EnqueueLocked(Action action);
    void ApplyDirectiveLocked(size_t index, unsigned round, bool missing, PowerKeywordAction directive);
    void StopLocked(State state, const wchar_t *reason);
    void Log(const Action &action, const std::wstring &result);
    void Work();
    void Done(const Action &action, Delivery delivery, const std::wstring &error);

    const std::uint64_t session_;
    const std::wstring target_;
    Match ready_;
    std::vector<RuleState> rules_;
    const Sender send_;
    const std::function<bool()> connected_;
    const Audit audit_;
    const std::shared_ptr<std::atomic_bool> permit_ = std::make_shared<std::atomic_bool>(true);
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    State state_ = State::Pending;
    State resumeState_ = State::Listening;
    unsigned round_ = 0;
    bool readySeen_ = false, timedOut_ = false;
    CycleAction pendingAction_ = CycleAction::Continue;
    Clock::time_point pausedAt_{};
    std::optional<Action> writing_;
    Clock::time_point writeDeadline_;
    std::wstring reason_;
    std::deque<Action> actions_;
    std::uint64_t nextSequence_ = 1;
    std::thread worker_;
};

} // namespace serialctl
