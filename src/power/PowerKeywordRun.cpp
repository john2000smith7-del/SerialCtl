#include "PowerKeywordRun.h"
#include <algorithm>

namespace serialctl {

const wchar_t *PowerKeywordActionLabel(PowerKeywordAction action) {
    switch (action) {
    case PowerKeywordAction::None: return L"无操作";
    case PowerKeywordAction::RunCommands: return L"执行命令";
    case PowerKeywordAction::StopTest: return L"停止测试";
    case PowerKeywordAction::StopTestPowerOff: return L"停止测试,下电";
    case PowerKeywordAction::PowerOffContinue: return L"下电,继续测试";
    case PowerKeywordAction::RunCommandsStopTest: return L"执行命令,停止测试";
    case PowerKeywordAction::PauseAll: return L"暂停当前所有动作";
    }
    return L"无操作";
}

const char *PowerKeywordActionKey(PowerKeywordAction action) {
    switch (action) {
    case PowerKeywordAction::None: return "none";
    case PowerKeywordAction::RunCommands: return "commands";
    case PowerKeywordAction::StopTest: return "stop";
    case PowerKeywordAction::StopTestPowerOff: return "stop_off";
    case PowerKeywordAction::PowerOffContinue: return "off_continue";
    case PowerKeywordAction::RunCommandsStopTest: return "commands_stop";
    case PowerKeywordAction::PauseAll: return "pause";
    }
    return "none";
}

PowerKeywordAction ParsePowerKeywordAction(const std::string &key) {
    if (key == "commands") return PowerKeywordAction::RunCommands;
    if (key == "stop") return PowerKeywordAction::StopTest;
    if (key == "stop_off") return PowerKeywordAction::StopTestPowerOff;
    if (key == "off_continue") return PowerKeywordAction::PowerOffContinue;
    if (key == "commands_stop") return PowerKeywordAction::RunCommandsStopTest;
    if (key == "pause") return PowerKeywordAction::PauseAll;
    return PowerKeywordAction::None;
}

PowerKeywordRun::Match::Match(std::string value) : pattern(std::move(value)), prefix(pattern.size()) {
    size_t length = 0;
    for (size_t i = 1; i < pattern.size(); ++i) {
        while (length && pattern[i] != pattern[length]) length = prefix[length - 1];
        if (pattern[i] == pattern[length]) ++length;
        prefix[i] = length;
    }
}

bool PowerKeywordRun::Match::Feed(unsigned char byte) {
    if (pattern.empty()) return false;
    const auto value = static_cast<char>(byte);
    while (matched && pattern[matched] != value) matched = prefix[matched - 1];
    if (pattern[matched] == value) ++matched;
    if (matched != pattern.size()) return false;
    matched = 0; // Count each non-overlapping occurrence once.
    return true;
}

PowerKeywordRun::RuleState::RuleState(Rule value)
    : rule(std::move(value)), keyword(std::move(rule.keyword)), begin(std::move(rule.begin)), end(std::move(rule.end)) {}

PowerKeywordRun::PowerKeywordRun(std::uint64_t session, std::wstring target, std::string ready, std::vector<Rule> rules,
                                 Sender send, std::function<bool()> connected, Audit audit)
    : session_(session), target_(std::move(target)), ready_(std::move(ready)), send_(std::move(send)),
      connected_(std::move(connected)), audit_(std::move(audit)) {
    for (auto &rule : rules) rules_.emplace_back(std::move(rule));
}

PowerKeywordRun::~PowerKeywordRun() {
    Cancel(L"任务已关闭");
    if (worker_.joinable()) worker_.join();
}

bool PowerKeywordRun::BeginRound() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Active() || !connected_()) return false;
    if (!worker_.joinable()) worker_ = std::thread(&PowerKeywordRun::Work, this);
    ++round_;
    ready_.matched = 0;
    readySeen_ = timedOut_ = false;
    if (pendingAction_ == CycleAction::PowerOffContinue) pendingAction_ = CycleAction::Continue;
    for (auto &entry : rules_) {
        entry.keyword.matched = entry.begin.matched = entry.end.matched = 0;
        entry.count.round = 0;
        entry.count.missRound = 0;
        entry.count.window = entry.begin.pattern.empty() ? Window::Open : Window::Waiting;
    }
    state_ = State::Listening;
    audit_(L"关键字测试 · " + target_ + L" · 第 " + std::to_wstring(round_) + L" 轮开始监听");
    return true;
}

void PowerKeywordRun::Feed(const Bytes &bytes) {
    if (!Active()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ != State::Listening) return;
    for (auto byte : bytes) {
        if (!readySeen_ && ready_.Feed(byte)) {
            readySeen_ = true;
            audit_(L"关键字测试 · " + target_ + L" · 第 " + std::to_wstring(round_) + L" 轮上电完成");
        }
        for (size_t i = 0; i < rules_.size(); ++i) {
            auto &entry = rules_[i];
            if (entry.count.window == Window::Waiting) {
                if (!entry.begin.Feed(byte)) continue;
                entry.count.window = Window::Open;
                // Include the start marker itself, including when it is also
                // the keyword. Bytes before the marker are outside the window.
                for (auto markerByte : entry.begin.pattern) {
                    if (entry.keyword.Feed(static_cast<unsigned char>(markerByte))) Hit(i);
                    if (entry.end.Feed(static_cast<unsigned char>(markerByte))) { Close(i); break; }
                }
            } else if (entry.count.window == Window::Open) {
                if (entry.keyword.Feed(byte)) Hit(i);
                if (entry.end.Feed(byte)) Close(i);
            }
            if (!Active()) return;
        }
    }
}

void PowerKeywordRun::Hit(size_t index) {
    auto &entry = rules_[index];
    ++entry.count.round;
    ++entry.count.total;
    const auto action = entry.rule.hitAction;
    if (action != PowerKeywordAction::None) Queue(index, false);
    else Log({index, round_, false}, L"仅计数");
}

void PowerKeywordRun::Close(size_t index) {
    auto &entry = rules_[index];
    entry.count.window = Window::Closed;
    if (entry.count.round == 0) {
        ++entry.count.missRound;
        ++entry.count.missTotal;
        const auto action = entry.rule.missAction;
        if (action != PowerKeywordAction::None) Queue(index, true);
        else Log({index, round_, true}, L"无操作");
    }
}

void PowerKeywordRun::EnqueueLocked(Action action) {
    if (!Active()) return;
    if (actions_.size() >= 65536) {
        ++rules_[action.index].count.failed;
        Log(action, L"失败：待发送队列已满");
        StopLocked(State::Failed, L"关键字动作积压，任务已停止");
        return;
    }
    action.sequence = nextSequence_++;
    actions_.push_back(std::move(action));
    Log(actions_.back(), L"已排队");
    wake_.notify_one();
}

void PowerKeywordRun::Queue(size_t index, bool missing) {
    if (!Active()) return;
    const auto &rule = rules_[index].rule;
    const auto actionType = missing ? rule.missAction : rule.hitAction;
    if (actionType == PowerKeywordAction::None) return;
    const auto baseDelay = std::chrono::milliseconds(missing ? rule.display.missDelayMs : rule.display.hitDelayMs);
    auto scheduled = Clock::now() + baseDelay;

    const bool sendsCommands = actionType == PowerKeywordAction::RunCommands ||
                               actionType == PowerKeywordAction::RunCommandsStopTest;
    if (sendsCommands) {
        const auto &commands = missing ? rule.missCommands : rule.hitCommands;
        const auto &legacyBytes = missing ? rule.onMiss : rule.onHit;
        const auto &legacyDisplay = missing ? rule.display.onMiss : rule.display.onHit;
        std::vector<Command> effective = commands;
        if (effective.empty() && !legacyBytes.empty())
            effective.push_back({legacyBytes, legacyDisplay, 0, 0});
        if (!effective.empty()) {
            for (size_t i = 0; i < effective.size() && Active(); ++i) {
                scheduled += std::chrono::milliseconds(effective[i].delayMs);
                Action item;
                item.index = index;
                item.round = round_;
                item.missing = missing;
                item.hasCommand = true;
                item.bytes = effective[i].bytes;
                item.commandDisplay = effective[i].display;
                item.due = scheduled;
                if (i + 1 == effective.size() && actionType == PowerKeywordAction::RunCommandsStopTest)
                    item.followUp = PowerKeywordAction::StopTest;
                EnqueueLocked(std::move(item));
                scheduled += std::chrono::milliseconds(effective[i].intervalMs);
            }
            return;
        }
        if (actionType == PowerKeywordAction::RunCommands) {
            Log({index, round_, missing}, L"未配置可执行命令，仅计数");
            return;
        }
    }

    Action control;
    control.index = index;
    control.round = round_;
    control.missing = missing;
    control.hasCommand = false;
    control.followUp = actionType == PowerKeywordAction::RunCommandsStopTest ? PowerKeywordAction::StopTest : actionType;
    control.due = scheduled;
    EnqueueLocked(std::move(control));
}

void PowerKeywordRun::ApplyDirectiveLocked(size_t index, unsigned round, bool missing, PowerKeywordAction directive) {
    if (!Active() || directive == PowerKeywordAction::None || directive == PowerKeywordAction::RunCommands) return;
    if (directive == PowerKeywordAction::StopTest || directive == PowerKeywordAction::RunCommandsStopTest) {
        pendingAction_ = CycleAction::StopKeepPower;
        reason_ = L"触发动作：停止测试";
        Log({index, round, missing}, L"触发停止测试");
        wake_.notify_all();
    } else if (directive == PowerKeywordAction::StopTestPowerOff) {
        pendingAction_ = CycleAction::StopPowerOff;
        reason_ = L"触发动作：停止测试并下电";
        Log({index, round, missing}, L"触发停止测试并下电");
        wake_.notify_all();
    } else if (directive == PowerKeywordAction::PowerOffContinue) {
        if (round == round_) {
            pendingAction_ = CycleAction::PowerOffContinue;
            Log({index, round, missing}, L"触发下电并继续测试");
            wake_.notify_all();
        }
    } else if (directive == PowerKeywordAction::PauseAll) {
        if (state_ != State::Paused) {
            resumeState_ = (state_ == State::Draining || state_ == State::BetweenRounds) ? state_ : State::Listening;
            state_ = State::Paused;
            pausedAt_ = Clock::now();
            pendingAction_ = CycleAction::Pause;
            reason_ = L"触发动作：已暂停当前所有动作";
            Log({index, round, missing}, L"已暂停当前所有动作");
            wake_.notify_all();
        }
    }
}

bool PowerKeywordRun::EndRound() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Paused) return false;
    if (state_ == State::Listening) {
        timedOut_ = !ready_.pattern.empty() && !readySeen_;
        if (timedOut_) audit_(L"关键字测试 · " + target_ + L" · 第 " + std::to_wstring(round_) + L" 轮上电标志超时");
        for (size_t i = 0; i < rules_.size() && Active(); ++i) {
            if (rules_[i].count.window == Window::Open) Close(i);
            else if (rules_[i].count.window == Window::Waiting) {
                ++rules_[i].count.missRound;
                ++rules_[i].count.missTotal;
                Log({i, round_, true}, L"未进入检测，无操作");
            }
        }
        if (Active() && state_ != State::Paused) state_ = State::Draining;
    }
    if (!Active() || state_ == State::Paused || !actions_.empty() || writing_ ||
        pendingAction_ == CycleAction::StopKeepPower || pendingAction_ == CycleAction::StopPowerOff)
        return false;
    state_ = State::BetweenRounds;
    return true;
}

bool PowerKeywordRun::Healthy() {
    if (Active() && !connected_()) Cancel(L"关联终端已断开，或独占传输、编码状态已变更");
    return Active();
}

PowerCycleObserver::CycleAction PowerKeywordRun::PollAction() {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto action = pendingAction_;
    if (action == CycleAction::PowerOffContinue)
        pendingAction_ = CycleAction::Continue;
    return action;
}

bool PowerKeywordRun::Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!Active() || state_ != State::Paused) return false;
    const auto delta = Clock::now() - pausedAt_;
    for (auto &item : actions_) item.due += delta;
    if (writing_) writeDeadline_ += delta;
    pendingAction_ = CycleAction::Continue;
    state_ = resumeState_;
    reason_.clear();
    audit_(L"关键字测试 · " + target_ + L" · 已恢复继续测试");
    wake_.notify_all();
    return true;
}

void PowerKeywordRun::Finish() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (Active()) StopLocked(State::Finished, L"测试完成");
}

void PowerKeywordRun::Cancel(const wchar_t *reason) {
    if (!permit_->exchange(false)) return;
    std::lock_guard<std::mutex> lock(mutex_);
    StopLocked(State::Canceled, reason);
}

void PowerKeywordRun::StopLocked(State state, const wchar_t *reason) {
    permit_->store(false);
    state_ = state;
    reason_ = reason;
    for (auto &rule : rules_) {
        if (rule.count.window == Window::Open) rule.count.window = Window::Interrupted;
    }
    for (const auto &action : actions_) {
        ++rules_[action.index].count.canceled;
        Log(action, L"已取消，未写出");
    }
    actions_.clear();
    audit_(L"关键字测试 · " + target_ + L" · " + reason_);
    wake_.notify_all();
}

PowerKeywordRun::Snapshot PowerKeywordRun::Status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot result{state_, round_, readySeen_, !ready_.pattern.empty(), timedOut_, target_, reason_, {}};
    for (const auto &rule : rules_) result.counts.push_back(rule.count);
    return result;
}

void PowerKeywordRun::Log(const Action &action, const std::wstring &result) {
    const auto &rule = rules_[action.index].rule.display;
    const std::wstring &command = !action.commandDisplay.empty() ? action.commandDisplay : (action.missing ? rule.onMiss : rule.onHit);
    const wchar_t *actionLabel = PowerKeywordActionLabel(action.missing ? rules_[action.index].rule.missAction : rules_[action.index].rule.hitAction);
    audit_(L"关键字测试 · " + target_ + L" · 第 " + std::to_wstring(action.round) + L" 轮 · " +
           (action.missing ? L"未捕获：" : L"捕获：") + rule.keyword +
           L" · 动作：" + actionLabel +
           (command.empty() ? L"" : L" · 指令：" + command) + L" · " + result);
}

void PowerKeywordRun::Work() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (Active()) {
        if (state_ == State::Paused) {
            wake_.wait(lock, [this] { return !Active() || state_ != State::Paused; });
            continue;
        }
        if (writing_) {
            if (!wake_.wait_until(lock, writeDeadline_, [this] { return !Active() || !writing_; })) {
                Log(*writing_, L"传输写入超时，写入结果未知");
                StopLocked(State::Failed, L"等待终端写入超时，已停止测试");
            }
            continue;
        }
        wake_.wait(lock, [this] { return !Active() || (state_ != State::Paused && !actions_.empty()); });
        if (!Active()) break;
        if (state_ == State::Paused) continue;
        const auto due = actions_.front().due;
        if (Clock::now() < due) {
            wake_.wait_until(lock, due);
            continue;
        }
        const auto action = actions_.front();
        actions_.pop_front();
        if (!action.hasCommand) {
            ApplyDirectiveLocked(action.index, action.round, action.missing, action.followUp);
            continue;
        }
        const auto bytes = action.bytes;
        writing_ = action;
        writeDeadline_ = Clock::now() + std::chrono::seconds(30);
        const auto weak = weak_from_this();
        auto complete = [weak, action](Delivery delivery, const std::wstring &error) {
            if (auto run = weak.lock()) run->Done(action, delivery, error);
        };
        lock.unlock();
        std::wstring error;
        bool accepted = false;
        try {
            accepted = send_(bytes, [permit = permit_] { return permit->load(); }, complete, error);
        } catch (const std::exception &) {
            error = L"发送指令时发生错误";
        }
        if (!accepted) Done(action, Delivery::NotWritten, error);
        lock.lock();
    }
}

void PowerKeywordRun::Done(const Action &action, Delivery delivery, const std::wstring &error) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &count = rules_[action.index].count;
    if (delivery == Delivery::Written) {
        ++count.written;
        Log(action, L"传输已写入，设备执行结果待确认");
        if (action.followUp != PowerKeywordAction::None)
            ApplyDirectiveLocked(action.index, action.round, action.missing, action.followUp);
    } else if (Active() || delivery == Delivery::Unknown) {
        ++count.failed;
        const auto reason = (delivery == Delivery::Unknown ? L"关键字指令写入结果未知：" : L"关键字指令未写出：") + error;
        Log(action, reason);
        if (Active()) StopLocked(State::Failed, reason.c_str());
    } else {
        ++count.canceled;
        Log(action, error.empty() ? L"任务已停止，写入结果未知" : error);
    }
    writing_.reset();
    wake_.notify_one();
}

} // namespace serialctl
