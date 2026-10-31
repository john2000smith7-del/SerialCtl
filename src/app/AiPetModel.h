#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace serialctl::pet {
enum class Activity { Offline, Connecting, Idle, Thinking, Working, Replying, Done, Failed, Stopped };
enum class Action { None, Idle, Wave, Boxing };
struct Settings {
    bool enabled = true, follow = true, opening = true;
    bool desktop = true, topmost = true, roaming = true;
    int desktopX = -2147483647, desktopY = -2147483647;
    int size = 144, x = 500, y = 500;
    void Normalize() {
        if (size != 96 && size != 120 && size != 144)
            size = 96;
        x = std::clamp(x, 0, 1000);
        y = std::clamp(y, 0, 1000);
    }
};
struct Sample {
    unsigned pose = 28;
    double x = 0, lift = 0, breath = 1;
    unsigned hop = 0;
    bool punching = false, manual = false, opening = false;
    std::wstring label;
};

// Pure monotonic-clock state machine. It has no model, network or device API.
// The Win32 component provides visibility and draws only this bounded result.
class RoamingClock {
  public:
    double Step(std::uint64_t now, bool allowed) {
        const auto delta = last_ && now >= last_ ? std::min<std::uint64_t>(now-last_,100) : 0;
        last_ = now;
        if (!allowed) return 0;
        phase_ = (phase_ + delta) % 12000;
        return phase_ < 7000 ? delta * .012 : 0;
    }
    unsigned Pose() const { constexpr unsigned gait[] = {4,5,7,6}; return gait[(phase_/160)%4]; }
  private:
    std::uint64_t last_ = 0, phase_ = 0;
};

class Model {
  public:
    static constexpr std::uint64_t OpeningDelay = 4000, BoxingDuration = 8000;
    const Settings &Preferences() const {
        return settings_;
    }
    Activity CurrentActivity() const {
        return activity_;
    }
    bool CanRoam() const {
        return settings_.enabled && settings_.desktop && settings_.roaming &&
            action_ == Action::None && !due_ &&
            (activity_ == Activity::Offline || activity_ == Activity::Idle || activity_ == Activity::Done);
    }
    Action CurrentAction() const {
        return action_;
    }
    bool Active() const {
        return (page_ || persistent_) && settings_.enabled;
    }
    void Persistent(bool value, std::uint64_t now) {
        persistent_ = value;
        if (!persistent_ && !page_) Stop(now);
    }
    std::uint64_t OpeningDue() const {
        return due_;
    }
    void Configure(Settings settings, std::uint64_t now) {
        settings.Normalize();
        settings_ = settings;
        if (!settings_.enabled || (!settings_.opening && opening_))
            Stop(now);
        if (!settings_.enabled || !settings_.opening)
            due_ = 0;
    }
    void Page(bool active, std::uint64_t now) {
        if (page_ == active)
            return;
        page_ = active;
        if (persistent_) {
            if (active && Active() && settings_.opening && action_ == Action::None) due_ = now + OpeningDelay;
            else if (!active) due_ = 0;
            return;
        }
        Stop(now);
        paused_ = false;
        if (Active() && settings_.opening)
            due_ = now + OpeningDelay;
    }
    void SetActivity(Activity value, std::uint64_t now) {
        if (activity_ == value)
            return;
        activity_ = value;
        activityAt_ = now;
    }
    void Play(Action action, std::uint64_t now, bool opening = false) {
        if (!Active())
            return;
        action_ = action;
        actionAt_ = now;
        opening_ = opening;
        due_ = 0;
        paused_ = false;
    }
    void Stop(std::uint64_t) {
        action_ = Action::None;
        opening_ = false;
        due_ = 0;
    }
    Sample At(std::uint64_t now, bool presentable = true) {
        if (!Active())
            return {};
        if (!presentable) {
            if (!paused_)
                pauseAt_ = now;
            paused_ = true;
            return Last(now);
        }
        if (paused_) {
            const auto delay = Age(now, pauseAt_);
            if (action_ != Action::None)
                actionAt_ += delay;
            activityAt_ += Age(now, std::max(pauseAt_, activityAt_));
            if (due_)
                due_ = now + OpeningDelay;
            paused_ = false;
        }
        if (due_ && now >= due_)
            Play(Action::Boxing, now, true);
        if (action_ != Action::None && Age(now, actionAt_) >= (action_ == Action::Boxing ? BoxingDuration : 1800)) {
            action_ = Action::None;
            opening_ = false;
        }
        return Last(now);
    }

  private:
    struct Key {
        unsigned pose;
        unsigned milliseconds;
    };
    template <size_t N> static unsigned Choose(unsigned time, const Key (&keys)[N]) {
        for (const auto &key : keys) {
            if (time < key.milliseconds)
                return key.pose;
            time -= key.milliseconds;
        }
        return keys[N - 1].pose;
    }
    static std::uint64_t Age(std::uint64_t now, std::uint64_t then) {
        return now >= then ? now - then : 0;
    }
    Sample Last(std::uint64_t now) const {
        Sample sample;
        sample.breath = 1 + .008 * std::sin(static_cast<double>(now % 4000) * 6.283185307179586 / 4000);
        if (action_ != Action::None) {
            sample.manual = true;
            sample.opening = opening_;
            const auto time = static_cast<unsigned>(Age(now, actionAt_));
            if (action_ == Action::Wave) {
                constexpr unsigned poses[] = {29, 30, 31, 30, 29, 30, 31, 30, 29};
                sample.pose = poses[std::min<unsigned>(time / 160, 8)];
                sample.label = L"我在，随时可以继续排查";
            } else if (action_ == Action::Boxing)
                sample = Boxing(time, opening_);
            else
                sample.label = L"陪伴中";
            return sample;
        }
        const auto activity = settings_.follow ? activity_ : Activity::Idle;
        const auto age = Age(now, activityAt_);
        switch (activity) {
        case Activity::Offline:
            sample.label = L"AI 未连接";
            break;
        case Activity::Connecting:
            sample.pose = 32;
            sample.label = L"正在连接 AI…";
            break;
        case Activity::Thinking:
            sample.pose = 32;
            sample.label = L"正在思考…";
            break;
        case Activity::Working:
            sample.pose = 33;
            sample.label = L"正在处理工具任务…";
            break;
        case Activity::Replying:
            sample.pose = 33;
            sample.label = L"正在整理回复…";
            break;
        case Activity::Done:
            if (age < 900) {
                sample.pose = 34;
                sample.lift = .10 * std::sin(age * 3.141592653589793 / 900);
            }
            if (age < 5500)
                sample.label = L"任务已完成";
            break;
        case Activity::Failed:
            sample.pose = 35;
            sample.label = L"任务遇到问题，请查看对话";
            break;
        case Activity::Stopped:
            sample.pose = 35;
            sample.label = L"任务已停止";
            break;
        case Activity::Idle:
            break;
        }
        return sample;
    }
    static Sample Boxing(unsigned time, bool opening) {
        Sample s;
        s.manual = true;
        s.opening = opening;
        s.breath = 1;
        s.pose = 17;
        s.label = opening ? L"开场动作" : L"动作演示中";
        if (time < 600)
            return s;
        if (time < 3300) {
            const unsigned hop = std::min(2u, (time - 600) / 900), local = time - 600 - hop * 900;
            const double u = std::clamp((static_cast<double>(local) - 120) / 600, 0.0, 1.0);
            constexpr double extent = 64.0 / 310;
            constexpr double start[] = {0, extent, -extent}, end[] = {extent, -extent, 0};
            s.x = start[hop] + (end[hop] - start[hop]) * (u * u * (3 - 2 * u));
            s.lift = (72.0 / 310) * std::sin(3.141592653589793 * u);
            constexpr Key keys[] = {{2, 70}, {3, 50}, {4, 90}, {5, 120}, {6, 150}, {7, 150}, {8, 130}, {9, 140}};
            s.pose = Choose(local, keys);
            s.hop = hop + 1;
            s.label = hop == 1 ? L"后跳 2 / 3" : hop == 0 ? L"前跳 1 / 3" : L"前跳 3 / 3";
        } else if (time < 3900)
            s.label = L"落地站稳";
        else if (time < 4900) {
            constexpr Key keys[] = {{10, 130}, {11, 100}, {12, 160}, {11, 100}, {10, 130}, {17, 380}};
            s.pose = Choose(time - 3900, keys);
            s.punching = s.pose == 11 || s.pose == 12;
            s.label = L"刺拳 · 单手出拳";
        } else if (time < 6000) {
            constexpr Key keys[] = {{24, 150}, {25, 130}, {26, 180}, {25, 130}, {27, 160}, {17, 350}};
            s.pose = Choose(time - 4900, keys);
            s.punching = s.pose == 25 || s.pose == 26;
            s.label = L"直拳 · 收拳护脸";
        } else if (time < 7150) {
            constexpr Key keys[] = {{17, 150}, {19, 150}, {20, 180}, {19, 130}, {17, 540}};
            s.pose = Choose(time - 6000, keys);
            s.punching = s.pose == 19 || s.pose == 20;
            s.label = L"摆拳 · 单手出拳";
        } else
            s.label = L"回防";
        return s;
    }
    Settings settings_;
    Activity activity_ = Activity::Offline;
    Action action_ = Action::None;
    bool page_ = false, persistent_ = false, opening_ = false, paused_ = false;
    std::uint64_t due_ = 0, actionAt_ = 0, activityAt_ = 0, pauseAt_ = 0;
};
} // namespace serialctl::pet
