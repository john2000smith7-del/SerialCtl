#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace serialctl::pet {
enum class Activity { Offline, Connecting, Idle, Thinking, Working, Replying, Done, Failed, Stopped };
enum class Action { None, Idle, Wave, Boxing, Hop, Dance, Cheer, Listen, Think, Work, Reply, Concern, Settle, Walk,
                    Stretch, Sleep, Groom, Treat };
enum class Character { Whale, Lulu, Cat };
inline const wchar_t *CharacterName(Character character) {
    constexpr const wchar_t *names[] = {L"鲸鱼娘", L"水豚噜噜", L"小猫"};
    return names[static_cast<unsigned>(character)];
}
struct PoseAction { Action action; const wchar_t *name; };
struct CharacterActions { const PoseAction *items; unsigned count; unsigned offset; };
inline CharacterActions ActionsFor(Character character) {
    static constexpr PoseAction whale[] = {
        {Action::Idle,L"待机"}, {Action::Wave,L"挥手"}, {Action::Listen,L"侧耳听"}, {Action::Think,L"托腮思考"},
        {Action::Work,L"操作笔电"}, {Action::Reply,L"递便笺"}, {Action::Cheer,L"举手欢呼"}, {Action::Concern,L"握手担心"},
        {Action::Settle,L"摆手暂停"}, {Action::Walk,L"走路"}, {Action::Hop,L"蹦跳"}, {Action::Dance,L"转圈跳舞"},
        {Action::Boxing,L"前后跳步＋拳击"}, {Action::Stretch,L"伸懒腰"}, {Action::Sleep,L"坐着打盹"}};
    static constexpr PoseAction lulu[] = {
        {Action::Idle,L"待机"}, {Action::Wave,L"挥手"}, {Action::Walk,L"走路"}, {Action::Listen,L"侧耳听"},
        {Action::Think,L"托腮思考"}, {Action::Work,L"敲键盘"}, {Action::Reply,L"举书展示"}, {Action::Cheer,L"跳起庆祝"},
        {Action::Concern,L"歪头担心"}, {Action::Settle,L"放下双手"}, {Action::Sleep,L"坐着打盹"}, {Action::Treat,L"吃冰淇淋"}};
    static constexpr PoseAction cat[] = {
        {Action::Idle,L"坐好"}, {Action::Listen,L"竖耳探身"}, {Action::Think,L"托腮"}, {Action::Work,L"敲键盘"},
        {Action::Reply,L"举纸抬头"}, {Action::Cheer,L"举爪跳起"}, {Action::Concern,L"歪头垂耳"}, {Action::Settle,L"收爪坐定"},
        {Action::Stretch,L"伸懒腰"}, {Action::Groom,L"舔爪洗脸"}, {Action::Walk,L"迈步"}, {Action::Sleep,L"蜷睡"}};
    if (character == Character::Lulu) return {lulu, 12, 15};
    if (character == Character::Cat) return {cat, 12, 27};
    return {whale, 15, 0};
}
inline unsigned PoseFor(Character character, Action action) {
    const auto actions = ActionsFor(character);
    for (unsigned i = 0; i < actions.count; ++i)
        if (actions.items[i].action == action) return 36 + actions.offset + i;
    return 36 + actions.offset;
}
struct Settings {
    Character character = Character::Whale;
    bool enabled = true, follow = true, opening = true;
    bool desktop = true, topmost = true, roaming = false;
    int desktopX = -2147483647, desktopY = -2147483647;
    int size = 144, x = 500, y = 500;
    void Normalize() {
        if (static_cast<unsigned>(character) > static_cast<unsigned>(Character::Cat)) character = Character::Whale;
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
    struct Position { double x = 0, y = 0; bool moving = false, facingLeft = false; };
    static constexpr double Radius = 40;
    Position Step(std::uint64_t now, bool allowed) {
        const auto delta = last_ && now >= last_ ? std::min<std::uint64_t>(now - last_, 100) : 0;
        last_ = now;
        if (!allowed || !delta) return {x_, y_};
        const auto walked = [](std::uint64_t t) { return t / 12000 * 7500 + std::min<std::uint64_t>(t % 12000, 7500); };
        const auto previous = walked(elapsed_);
        elapsed_ += delta;
        const auto walking = walked(elapsed_);
        if (walking == previous) return {x_, y_};
        const double t = static_cast<double>(walking) / 3000;
        const double radius = Radius * (.65 + .35 * std::cos(t * .61));
        const double easeIn = std::min(3 * t, 1.0);
        const double x = radius * std::sin(t) * easeIn;
        y_ = radius * std::sin(t * .5) * std::cos(t) * easeIn;
        const bool facingLeft = x < x_;
        x_ = x;
        return {x_, y_, true, facingLeft};
    }
    unsigned Pose() const { constexpr unsigned gait[] = {4, 5, 7, 6}; return gait[(elapsed_ / 160) % 4]; }
  private:
    std::uint64_t last_ = 0, elapsed_ = 0;
    double x_ = 0, y_ = 0;
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
        if (settings.character != settings_.character) Stop(now);
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
        if (due_ && now >= due_) {
            const auto entrance = settings_.character == Character::Whale ? Action::Boxing :
                                  settings_.character == Character::Lulu ? Action::Wave : Action::Stretch;
            if (activity_ == Activity::Offline || activity_ == Activity::Idle || activity_ == Activity::Done)
                Play(entrance, now, true);
            else
                due_ = 0;
        }
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
            if (settings_.character != Character::Whale ||
                (action_ != Action::Wave && action_ != Action::Boxing && action_ != Action::Hop &&
                 action_ != Action::Dance && action_ != Action::Cheer)) {
                const auto actions = ActionsFor(settings_.character);
                const auto chosen = action_ == Action::Wave && settings_.character == Character::Cat ? Action::Groom : action_;
                sample.pose = PoseFor(settings_.character, chosen);
                sample.label = actions.items[sample.pose - 36 - actions.offset].name;
                Motion(chosen, time, sample);
            } else if (action_ == Action::Wave) {
                constexpr unsigned poses[] = {29, 30, 31, 30, 29, 30, 31, 30, 29};
                sample.pose = poses[std::min<unsigned>(time / 160, 8)];
                sample.label = L"我在，随时可以继续排查";
            } else if (action_ == Action::Boxing)
                sample = Boxing(time, opening_);
            else if (action_ == Action::Hop) {
                constexpr unsigned poses[] = {28, 2, 3, 4, 6, 8, 9, 28, 28};
                sample.pose = poses[std::min<unsigned>(time / 100, 8)];
                sample.lift = .18 * std::sin((time % 900) * 3.141592653589793 / 900);
                sample.label = L"蹦跳";
            } else if (action_ == Action::Dance) {
                constexpr unsigned poses[] = {28, 4, 5, 7, 28, 7, 5, 4};
                sample.pose = poses[(time / 150) % 8];
                sample.x = .16 * std::sin(time * 6.283185307179586 / 1200);
                sample.label = L"跳舞";
            } else if (action_ == Action::Cheer) {
                sample.pose = (time / 250) % 2 ? 31 : 34;
                sample.lift = .12 * std::sin((time % 600) * 3.141592653589793 / 600);
                sample.label = L"欢呼";
            } else
                sample.label = L"陪伴中";
            return sample;
        }
        const auto activity = settings_.follow ? activity_ : Activity::Idle;
        const auto age = Age(now, activityAt_);
        Action pose = Action::Idle;
        switch (activity) {
        case Activity::Offline:
            sample.label = L"AI 未连接";
            break;
        case Activity::Connecting:
            pose = Action::Listen;
            sample.label = L"正在连接 AI…";
            break;
        case Activity::Thinking:
            pose = Action::Think;
            sample.label = L"正在思考…";
            break;
        case Activity::Working:
            pose = Action::Work;
            sample.label = L"正在处理工具任务…";
            break;
        case Activity::Replying:
            pose = Action::Reply;
            sample.label = L"正在整理回复…";
            break;
        case Activity::Done:
            if (age < 900) {
                pose = Action::Cheer;
                sample.lift = .10 * std::sin(age * 3.141592653589793 / 900);
            }
            if (age < 5500)
                sample.label = L"任务已完成";
            break;
        case Activity::Failed:
            pose = Action::Concern;
            sample.label = L"任务遇到问题，请查看对话";
            break;
        case Activity::Stopped:
            pose = Action::Settle;
            sample.label = L"任务已停止";
            break;
        case Activity::Idle:
            if (settings_.follow && age >= 45000) pose = Action::Sleep;
            else if (settings_.follow && age >= 12000 && age % 12000 < 1700) {
                pose = settings_.character == Character::Lulu ? Action::Treat :
                       settings_.character == Character::Cat && age % 24000 >= 12000 ? Action::Groom : Action::Stretch;
            }
            break;
        }
        sample.pose = PoseFor(settings_.character, pose);
        if (pose != Action::Cheer) Motion(pose, age, sample);
        return sample;
    }
    static void Motion(Action action, std::uint64_t time, Sample &sample) {
        const auto phase = [](std::uint64_t tick, unsigned period) {
            return (tick % period) * 6.283185307179586 / period;
        };
        if (action == Action::Cheer || action == Action::Walk) {
            sample.lift = (action == Action::Cheer ? .15 : .045) * std::abs(std::sin(phase(time, 760)));
            sample.x = .05 * std::sin(phase(time, 1050));
        } else if (action == Action::Wave || action == Action::Listen || action == Action::Reply ||
                   action == Action::Groom || action == Action::Treat) {
            sample.x = .025 * std::sin(phase(time, 900));
            sample.breath += .025 * std::sin(phase(time, 900));
        } else if (action == Action::Work) {
            sample.lift = .012 * std::abs(std::sin(phase(time, 300)));
            sample.x = .009 * std::sin(phase(time, 625));
        } else if (action == Action::Stretch || action == Action::Sleep) {
            sample.breath += .045 * std::sin(phase(time, action == Action::Sleep ? 2600 : 1500));
        }
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
