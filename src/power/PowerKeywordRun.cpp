#include "PowerKeywordRun.h"

namespace serialctl {

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
    for (auto &entry : rules_) {
        entry.keyword.matched = entry.begin.matched = entry.end.matched = 0;
        entry.count.round = 0;
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
    if (!entry.rule.onHit.empty()) Queue(index, false);
    else Log({index, round_, false, {}}, L"仅计数");
}

void PowerKeywordRun::Close(size_t index) {
    auto &entry = rules_[index];
    entry.count.window = Window::Closed;
    if (entry.count.round == 0) {
        if (!entry.rule.onMiss.empty()) Queue(index, true);
        else Log({index, round_, true, {}}, L"无操作");
    }
}

void PowerKeywordRun::Queue(size_t index, bool missing) {
    if (!Active()) return;
    const auto &rule = rules_[index].rule.display;
    const auto delay = std::chrono::milliseconds(missing ? rule.missDelayMs : rule.hitDelayMs);
    const Action action{index, round_, missing, Clock::now() + delay};
    // Bound outstanding memory. An overload is a test failure, never a silent
    // skipped hit or a per-rule execution count limit.
    if (actions_.size() >= 65536) {
        ++rules_[index].count.failed;
        Log(action, L"失败：待发送队列已满");
        StopLocked(State::Failed, L"关键字动作积压，任务已停止");
        return;
    }
    actions_.push_back(action);
    Log(action, L"已排队");
    wake_.notify_one();
}

bool PowerKeywordRun::EndRound() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == State::Listening) {
        timedOut_ = !ready_.pattern.empty() && !readySeen_;
        if (timedOut_) audit_(L"关键字测试 · " + target_ + L" · 第 " + std::to_wstring(round_) + L" 轮上电标志超时");
        for (size_t i = 0; i < rules_.size() && Active(); ++i) {
            if (rules_[i].count.window == Window::Open) Close(i);
            else if (rules_[i].count.window == Window::Waiting)
                Log({i, round_, true, {}}, L"未进入检测，无操作");
        }
        if (Active()) state_ = State::Draining;
    }
    if (!Active() || !actions_.empty() || writing_) return false;
    state_ = State::BetweenRounds;
    return true;
}

bool PowerKeywordRun::Healthy() {
    if (Active() && !connected_()) Cancel(L"关联终端已断开，或独占传输、编码状态已变更");
    return Active();
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
    const auto &command = action.missing ? rule.onMiss : rule.onHit;
    audit_(L"关键字测试 · " + target_ + L" · 第 " + std::to_wstring(action.round) + L" 轮 · " +
           (action.missing ? L"未捕获：" : L"捕获：") + rule.keyword +
           (command.empty() ? L"" : L" · 指令：" + command) + L" · " + result);
}

void PowerKeywordRun::Work() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (Active()) {
        if (writing_) {
            if (!wake_.wait_until(lock, writeDeadline_, [this] { return !Active() || !writing_; })) {
                Log(*writing_, L"传输写入超时，写入结果未知");
                StopLocked(State::Failed, L"等待终端写入超时，已停止测试");
            }
            continue;
        }
        wake_.wait(lock, [this] { return !Active() || !actions_.empty(); });
        if (!Active()) break;
        const auto due = actions_.front().due;
        if (Clock::now() < due) {
            wake_.wait_until(lock, due);
            continue;
        }
        const auto action = actions_.front();
        actions_.pop_front();
        const auto &rule = rules_[action.index].rule;
        const auto &bytes = action.missing ? rule.onMiss : rule.onHit;
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
