#pragma once
#include "AiReplyMarkup.h"
#include "CodexSession.h"
#include "AiPetModel.h"
#include "ToolbarIcons.h"
#include "UiStyle.h"
#include "AiAttachment.h"
#include "AiVoiceInput.h"
#include "BackgroundCleanup.h"
#include <shellapi.h>
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
    void FocusInput();
    // Begin on the UI thread; wait on the owning cleanup thread.
    void BeginShutdown();
    void WaitForShutdown();
    void Shutdown();
    void Disconnect();
    bool Connected() const {
        return client_.Ready();
    }
    bool Busy() const {
        return busy_ || client_.Running();
    }
    bool TaskRunning() const {return busy_;}
    void RefreshTargets();
    void SelectTarget(const std::string &id);
    const std::string& AssociatedTarget() const { return busy_ ? turnTarget_ : target_; }
    void AttachSelection(const std::wstring &text, const std::string &sessionId);
    bool PreTranslate(MSG &);
    void SendPrompt(std::wstring text = {});
    void SetComposerWidth(int width);
    void Stop();
    std::string WorkspacePractices() const { return workspacePractices_; }

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
        std::vector<ai::markup::Span> spans;
        size_t block = SIZE_MAX;
        bool tabAfter = false;
    };
    struct ReplyBlock {
        ai::markup::Kind kind = ai::markup::Kind::Prose;
        RECT rect{}, body{}, copy{}, track{};
        size_t source = 0, codeBegin = 0, codeEnd = 0;
        int offset = 0, maximum = 0;
        std::wstring language;
        std::vector<int> columns, rows;
    };
    struct Message {
        std::wstring role, text, streamedOutput, nativeBase, technical;
        std::string item, group;
        bool process = false, hidden = false, folded = false, groupHeader = false;
        RECT bounds{};
        std::vector<Paragraph> paragraphs;
        std::vector<ReplyBlock> blocks;
        size_t measuredLength = SIZE_MAX;
        int measuredWidth = 0, measuredHeight = 0;
        bool measuredGroupHeader = false;
    };
    struct HostPrompt {
        HostKeyChallenge challenge;
        std::promise<bool> answer;
    };
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK TranscriptProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK InputProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK SendButtonProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static INT_PTR CALLBACK ConnectProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK WorkspaceProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK ValueProc(HWND, UINT, WPARAM, LPARAM);
    HWND Child(const wchar_t *, const wchar_t *, int, DWORD = 0);
    void Layout();
    void Paint(HDC);
    void PaintTranscript(HDC);
    void LayoutMessage(HDC, Message &, int width);
    void PaintMessageText(HDC, const Message &, size_t index, const RECT &box, const RECT &viewport);
    bool TranscriptBlockPress(POINT contentPoint);
    bool TranscriptHorizontal(POINT clientPoint, int pixels);
    void DragTranscriptBlock(int clientX);
    RECT ReplyScrollThumb(const ReplyBlock &) const;
    static std::wstring ReplyCodeText(const Message &, const ReplyBlock &);
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
    void UpdateEffectiveAccess(const Json &result);
    void ServerRequest(const Json &);
    bool Request(ai::Operation, Json, const std::string &purpose);
    void Reply(const Json &, Json, bool success = true);
    void StartTurn();
    void NewConversation();
    bool ChooseWorkspace();
    void BeginWorkspaceConversation(const std::wstring&);
    std::wstring pendingWorkspace_, pendingHistoryWorkspace_;
    std::string pendingHistoryTarget_;
    bool pendingHistoryRestore_ = false;
    void BrowseDirectory(const std::wstring &, bool initial = false);
    void DirectoryResult(const Json &, bool fallback);
    void PopulateDirectories(bool updatePath = false);
    void QueueDirectorySelection(HWND);
    bool DirectoryMessage(HWND, UINT, WPARAM);
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
    bool Copy(const std::wstring &, const wchar_t *successNotice = L"已复制选中文字");
    TextPoint HitText(POINT) const;
    std::wstring SelectedText() const;
    void ClearTextSelection();
    int MeasureInputHeight(int textWidth) const;
    int InputLineHeight() const;
    int ComposerWidth() const;
    bool UseExpandedComposer(int width) const;
    void RefreshInputLayout();
    void ShowComposerMenu();
    void ToggleVoice();
    void PollVoice();
    void SetVoicePreview(const std::wstring &);
    bool InsertVoiceDraft(const std::wstring &);
    void UpdateVoiceControls();
    std::wstring VoicePhaseLabel() const;
    bool VoiceBusy() const { return voice_.Status() != ai::VoiceInput::State::Idle; }
    void ChooseAttachments(bool images);
    void LoadAttachments(std::vector<std::wstring> paths);
    void DropAttachments(HDROP drop);
    void PollAttachments();
    void BeginAttachmentUpload();
    void ContinueAttachmentUpload();
    void AttachmentResponse(const Json &event);
    void FailAttachmentUpload(const std::wstring &message);
    RECT InputScrollThumb() const;
    void LoadWorkspacePractices(bool forceReload = false);
    bool SaveWorkspacePractice(const std::wstring &entry);
    bool RollbackWorkspacePractices();
    bool showTurnProgress_ = false, needsPassword_ = false, quickObserve_ = false;
    bool deferReflow_ = false, reflowPending_ = false, appendPending_ = false;
    bool polling_ = false;
    int currentInputHeight_ = 0;
    int composerMaxWidth_ = 0;
    bool composerExpanded_ = false;
    bool layoutRunning_ = false, inputScrollDragging_ = false;
    RECT inputScrollLane_{};
    int inputScrollGrab_ = 0, inputWheelRemainder_ = 0;
    struct AttachmentLoad {
        std::atomic_bool done{false}, cancel{false};
        std::vector<ai::Attachment> files;
        std::vector<std::wstring> errors;
    };
    BackgroundCleanup attachmentLoader_;
    std::shared_ptr<AttachmentLoad> attachmentLoad_;
    std::vector<ai::Attachment> attachments_, sendingAttachments_;
    bool attachmentsStaged_ = false;
    std::uint64_t attachmentUploadToken_ = 0;
    size_t uploadFile_ = 0, uploadOffset_ = 0;
    std::string attachmentDirectory_;
    Json attachmentImagePaths_ = Json::array();
    std::wstring submittedDraft_;
    ULONGLONG attachmentDeadline_ = 0;
    bool keepDirectoryDropdownOpen_ = false;
    std::wstring requestedAccess_ = L"Full Access", effectiveAccess_ = L"待验证";
    std::string workspacePractices_, currentAgentsMdContent_, rollbackAgentsMdSnapshot_;
    bool hasRollbackSnapshot_ = false;
    bool agentsSnapshotValid_ = false, agentsFileExists_ = false, rollbackAgentsFileExists_ = false;
    bool agentsWritePending_ = false, pendingAgentsExists_ = false;
    std::string pendingAgentsContent_;
    std::vector<std::string> verifiedPractices_;
    bool autoWorkspacePractices_ = true;
    RECT composerBounds_{}, waitingBounds_{};
    std::string displayGroup_;
    std::set<std::string> collapsedGroups_;
    std::set<std::string> initializedGroups_;
    std::map<std::string, std::wstring> groupStates_;
    std::map<std::string, ULONGLONG> groupDurations_;
    std::vector<std::wstring> targetLabels_;
    std::wstring compactTargetLabel_, targetTooltip_, workspaceTooltip_, modelTooltip_, sendTooltip_, voiceTooltip_;
    int sendGesture_ = -1;
    UINT composerLogChoice_ = 0;
    Json targetSnapshot_;
    HWND tips_ = nullptr;
    void UpdateTargetLabel();
    HFONT ParagraphFont(int style, ai::markup::Ink ink = ai::markup::Ink::Plain) const;
    ToolbarIcons icons_;
    TextPoint selectionStart_{SIZE_MAX, 0, 0}, selectionEnd_{SIZE_MAX, 0, 0};
    bool selecting_ = false;
    size_t draggedBlockMessage_ = SIZE_MAX, draggedBlock_ = SIZE_MAX;
    int blockThumbGrab_ = 0;
    bool PromptValue(const std::wstring &, std::wstring &);
    HWND Control(int id) const {
        return GetDlgItem(window_, id);
    }
    int D(int n) const {
        return MulDiv(n, dpi_, 96);
    }
    HWND window_ = nullptr, transcript_ = nullptr, dialog_ = nullptr;
    HFONT font_ = nullptr, titleFont_ = nullptr, codeFont_ = nullptr, smallFont_ = nullptr, boldFont_ = nullptr;
    HBRUSH surfaceBrush_ = nullptr, fieldBrush_ = nullptr;
    bool dark_ = false;
    int dpi_ = 96;
    UiFieldStyle style_{&dark_, nullptr, 96};
    ai::CodexSession client_;
    ai::ServerSettings settings_;
    ai::VoiceInput voice_;
    ai::VoiceInput::State voiceShown_ = ai::VoiceInput::State::Idle;
    std::string voiceThread_;
    std::wstring voiceWorkspace_;
    std::wstring voiceNotice_;
    std::wstring workspaceDirectory_, directoryPath_, verifiedDirectory_, serverHome_, pendingDirectorySelection_;
    bool directoryRowsDirty_ = false, directoryPathDirty_ = false, workspaceVerified_ = false;
    Json directoryChildren_ = Json::array();
    HWND workspaceDialog_ = nullptr;
    bool directoryPending_ = false, directoryInitial_ = false, workspaceAccept_ = false;
    ULONGLONG directoryDeadline_ = 0;
    std::string pendingTargetSelection_;
    bool pendingTargetConnect_ = false, targetRefreshDeferred_ = false, targetCommitQueued_ = false;
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
        std::wstring workspace;
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
