#pragma once
#include <cstdint>
#include <cstddef>
#include <utility>
#include <string>
#include <vector>
namespace serialctl {
struct CommandItem {
    std::wstring name;
    std::vector<std::wstring> commands;
    std::uint32_t intervalMs = 500;
    CommandItem() = default;
    CommandItem(std::wstring n, std::wstring command) : name(std::move(n)), commands{std::move(command)} {}
};
// UI-thread domain model: no HWND, timers, GUI fields or retained UI callbacks.
// A sequence owns an immutable submission plan bound to one session.
class CommandMacroController {
public:
    static constexpr std::size_t MaximumSteps = 10;
    static constexpr std::uint32_t MaximumIntervalMs = 60000;
    enum class End { Silent, Submitted, UserStopped, Interrupted };
    struct Completion {
        bool active = false, error = false;
        std::uint64_t session = 0;
        std::size_t accepted = 0, total = 0;
        std::wstring message;
    };
    const std::vector<CommandItem>& Items() const { return items_; }
    bool Replace(std::vector<CommandItem> items, bool dirty = true);
    bool Update(std::size_t index, CommandItem item);
    bool Append(CommandItem item);
    bool Erase(std::size_t index);
    bool Move(std::size_t from, std::size_t to);
    bool LoadFile(const std::wstring&, std::wstring& error);
    bool SaveFile(const std::wstring&, std::wstring& error) const;
    bool ImportFile(const std::wstring& source, const std::wstring& protectedPath, std::wstring& error);
    bool Dirty() const { return dirty_; }
    void MarkSaved() { dirty_ = false; }
    void MarkDirty() { dirty_ = true; }
    bool FileProtected() const { return protected_; }
    void ProtectFile(bool value) { protected_ = value; }
    bool Running() const { return runningIndex_ >= 0; }
    int RunningIndex() const { return runningIndex_; }
    std::size_t Accepted() const { return accepted_; }
    std::size_t Total() const { return plan_.commands.size(); }
    std::uint64_t Session() const { return session_; }
    std::uint32_t CodePage() const { return codePage_; }
    std::uint32_t Interval() const { return plan_.intervalMs; }
    const std::wstring& Name() const { return plan_.name; }
    bool Duplicate(std::size_t index, std::uint64_t session, std::uint64_t now, std::uint32_t window) const;
    void Activated(std::size_t index, std::uint64_t session, std::uint64_t now);
    void ClearActivation() { lastActivation_ = 0; lastIndex_ = -1; }
    bool Begin(std::size_t index, std::uint64_t session, std::uint32_t codePage, std::wstring lineEnding);
    bool BeginPlan(CommandItem plan, std::uint64_t session, std::uint32_t codePage, std::wstring lineEnding);
    bool Next(std::uint64_t currentSession, bool connected, std::wstring& text) const;
    void Accept() { if (Running() && accepted_ < Total()) ++accepted_; }
    bool Complete() const { return Running() && accepted_ == Total(); }
    Completion Stop(End end, std::wstring detail = {});
private:
    std::vector<CommandItem> items_;
    bool dirty_ = false, protected_ = false;
    int runningIndex_ = -1, lastIndex_ = -1;
    std::uint64_t session_ = 0, lastSession_ = 0, lastActivation_ = 0;
    std::uint32_t codePage_ = 0;
    std::size_t accepted_ = 0;
    CommandItem plan_;
    std::wstring lineEnding_;
};
} // namespace serialctl
