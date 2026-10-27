#pragma once
#include "CodexSession.h"
#include "AiPetModel.h"
#include "ToolbarIcons.h"
#include "UiStyle.h"
#include <atomic>
#include <deque>
#include <future>
#include <map>
#include <set>
#include <tuple>

namespace serialctl {
inline constexpr UINT AiStateChanged = WM_APP + 22;
inline constexpr UINT AiTargetChanged = WM_APP + 23;
class AiPane {
  public:
    using Json = ai::Json;
    using DeviceRequest = std::function<Json(const std::string &, const Json &)>;
    ~AiPane();
    bool Create(HWND parent, HFONT font, bool dark, DeviceRequest devices);
    HWND Handle() const {
        return window_;
    }
    pet::Activity CompanionActivity() const { return companionActivity_; }
    void Theme(bool dark);
    void ShowConnection();
    void Shutdown();
    void Disconnect();
    bool Connected() const {
        return client_.Ready();
    }
    bool Busy() const {
        return busy_ || client_.Running();
    }
    void RefreshTargets();
    void SelectTarget(const std::string &id);
    const std::string& AssociatedTarget() const { return busy_ ? turnTarget_ : target_; }
    void AttachSelection(const std::wstring &text, const std::string &sessionId);
    bool PreTranslate(MSG &);

  private:
#ifdef SERIALCTL_AI_TESTING
    friend struct AiPaneTestAccess;
#endif
    void SetCompanionActivity(pet::Activity);
    pet::Activity companionActivity_ = pet::Activity::Offline;
    struct TextLine {
        RECT rect{};
        size_t start = 0, length = 0;
        std::vector<int> advances;
    };
    using TextPoint = std::tuple<size_t, size_t, size_t>;
    struct Paragraph {
        RECT rect;
        std::wstring text;
        int style;
        std::vector<TextLine> lines;
    };
    struct Message {
        std::wstring role, text, streamedOutput, nativeBase, technical;
        std::string item, group;
        bool process = false, hidden = false, folded = false, groupHeader = false;
        RECT bounds{};
        std::vector<Paragraph> paragraphs;
        size_t measuredLength = SIZE_MAX;
        int measuredWidth = 0, measuredHeight = 0;
    };
    struct HostPrompt {
        HostKeyChallenge challenge;
        std::promise<bool> answer;
    };
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK TranscriptProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK InputProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static INT_PTR CALLBACK ConnectProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK WorkspaceProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK ValueProc(HWND, UINT, WPARAM, LPARAM);
    HWND Child(const wchar_t *, const wchar_t *, int, DWORD = 0);
    void Layout();
    void Paint(HDC);
    void PaintTranscript(HDC);
    void Reflow(bool appended = false);
    void UpdateTranscriptActions();
    void ShowTechnicalDetails(size_t);
    void AddTool(const std::string &, const Json &, const Json &, const std::wstring &target = {});
    void CollapseCompletedProcess(const std::string &group);
    void Scroll(int pixels);
    RECT ScrollThumb() const;
    void Action(int);
    void Poll();
    void QueueEvent(Json);
    void Event(const Json &);
    void ServerRequest(const Json &);
    bool Request(ai::Operation, Json, const std::string &purpose);
    void Reply(const Json &, Json, bool success = true);
    void SendPrompt(std::wstring text = {});
    void StartTurn();
    void Stop();
    void NewConversation();
    bool ChooseWorkspace();
    void BrowseDirectory(const std::wstring &, bool initial = false);
    void DirectoryResult(const Json &, bool fallback);
    void PopulateDirectories();
    void DirectoryFailure(const std::wstring &);
    void SaveWorkspaceDefault(const std::wstring &);
    void ShowDisplayLimit();
    bool displayLimitReached_ = false;
    void Add(std::wstring role, std::wstring text, std::string item = {}, std::wstring technical = {});
    void SetStatus(const std::wstring &);
    void UpdateActions();
    void ShowHistory();
    void RememberThread(const std::string &, const std::wstring &);
    Json History() const;
    void Copy(const std::wstring &);
    TextPoint HitText(POINT) const;
    std::wstring SelectedText() const;
    void ClearTextSelection();
    bool waitingReply_ = false, needsPassword_ = false;
    bool deferReflow_ = false, reflowPending_ = false, appendPending_ = false;
    bool polling_ = false;
    RECT composerBounds_{}, waitingBounds_{};
    std::string displayGroup_;
    std::set<std::string> collapsedGroups_;
    std::set<std::string> initializedGroups_;
    std::map<std::string, std::wstring> groupStates_;
    std::map<std::string, ULONGLONG> groupDurations_;
    std::vector<std::wstring> targetLabels_;
    std::wstring compactTargetLabel_, targetTooltip_;
    Json targetSnapshot_;
    HWND tips_ = nullptr;
    void UpdateTargetLabel();
    HFONT ParagraphFont(int style) const;
    ToolbarIcons icons_;
    TextPoint selectionStart_{SIZE_MAX, 0, 0}, selectionEnd_{SIZE_MAX, 0, 0};
    bool selecting_ = false;
    ULONGLONG copiedUntil_ = 0;
    bool PromptValue(const std::wstring &, std::wstring &);
    HWND Control(int id) const {
        return GetDlgItem(window_, id);
    }
    int D(int n) const {
        return MulDiv(n, dpi_, 96);
    }
    HWND window_ = nullptr, transcript_ = nullptr, dialog_ = nullptr;
    HFONT font_ = nullptr, titleFont_ = nullptr, codeFont_ = nullptr, smallFont_ = nullptr;
    HBRUSH surfaceBrush_ = nullptr, fieldBrush_ = nullptr;
    bool dark_ = false;
    int dpi_ = 96;
    UiFieldStyle style_{&dark_, nullptr, 96};
    ai::CodexSession client_;
    ai::ServerSettings settings_;
    std::wstring workspaceDirectory_, directoryPath_, verifiedDirectory_, serverHome_;
    Json directoryChildren_ = Json::array();
    HWND workspaceDialog_ = nullptr;
    bool directoryPending_ = false, directoryInitial_ = false, workspaceAccept_ = false;
    ULONGLONG directoryDeadline_ = 0;
    DeviceRequest devices_;
    std::atomic<bool> closing_{false};
    std::mutex eventsMutex_;
    std::deque<Json> events_;
    size_t eventBytes_ = 0;
    bool eventOverflowed_ = false; // protected by eventsMutex_
    std::shared_ptr<HostPrompt> hostPrompt_;
    std::vector<Message> messages_;
    ai::ChatScroll scroll_;
    bool dragging_ = false;
    int thumbGrab_ = 0, wheelRemainder_ = 0, layoutWidth_ = 0;
    size_t selectedMessage_ = SIZE_MAX;
    std::uint64_t nextRequest_ = 1;
    struct PendingRequest {
        std::string purpose, turn;
    };
    std::map<std::uint64_t, PendingRequest> requests_;
    std::set<std::string> seenToolCalls_;
    std::set<std::string> finishedTurns_;
    std::set<std::string> nativeItems_;
    std::string thread_, turn_, target_, turnTarget_, serverDefaultModel_;
    std::vector<std::string> targets_;
    std::wstring status_ = L"AI 未连接", pendingPrompt_, attachment_, valueTitle_, valueDraft_, turnTargetLabel_;
    std::string attachmentTarget_;
    bool busy_ = false, stopRequested_ = false, observedTool_ = false, dialogEnter_ = false, connectionError_ = false;
    bool taskAuthorized_ = false, restoreOnConfig_ = false;
    bool recoveryUsed_ = false;
    size_t messageBytes_ = 0;
    Json pendingObservation_;
    Json powerContext_;
    ULONGLONG observeDeadline_ = 0, completionDeadline_ = 0, turnStarted_ = 0;
};
} // namespace serialctl
