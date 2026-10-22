#pragma once
#include "CodexSession.h"
#include "UiStyle.h"
#include <atomic>
#include <deque>
#include <future>
#include <map>
#include <set>

namespace serialctl {
inline constexpr UINT AiStateChanged = WM_APP + 22;
class AiPane {
  public:
    using Json = ai::Json;
    using DeviceRequest = std::function<Json(const std::string &, const Json &)>;
    ~AiPane();
    bool Create(HWND parent, HFONT font, bool dark, DeviceRequest devices);
    HWND Handle() const {
        return window_;
    }
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
    void AttachSelection(const std::wstring &text, const std::string &sessionId);
    bool PreTranslate(MSG &);

  private:
#ifdef SERIALCTL_AI_TESTING
    friend struct AiPaneTestAccess;
#endif
    struct Paragraph {
        RECT rect;
        std::wstring text;
        int style;
    };
    struct Message {
        std::wstring role, text;
        std::string item;
        RECT bounds{};
        std::vector<Paragraph> paragraphs;
        size_t measuredLength = SIZE_MAX;
        int measuredWidth = 0, measuredHeight = 0;
    };
    struct HostPrompt {
        std::wstring host, fingerprint;
        unsigned port;
        std::promise<bool> answer;
    };
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK TranscriptProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK InputProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static INT_PTR CALLBACK ConnectProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK ValueProc(HWND, UINT, WPARAM, LPARAM);
    HWND Child(const wchar_t *, const wchar_t *, int, DWORD = 0);
    void Layout();
    void Paint(HDC);
    void PaintTranscript(HDC);
    void Reflow(bool appended = false);
    void Scroll(int pixels);
    RECT ScrollThumb() const;
    void Action(int);
    void Poll();
    void Event(const Json &);
    void ServerRequest(const Json &);
    bool Request(ai::Operation, Json, const std::string &purpose);
    void Reply(const Json &, Json, bool success = true);
    void SendPrompt(std::wstring text = {});
    void StartTurn();
    void Stop();
    void NewConversation();
    void Add(std::wstring role, std::wstring text, std::string item = {});
    void SetStatus(const std::wstring &);
    void UpdateActions();
    void ShowHistory();
    void RememberThread(const std::string &, const std::wstring &);
    Json History() const;
    void RefreshModels();
    void SelectModel();
    void Copy(const std::wstring &);
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
    DeviceRequest devices_;
    std::atomic<bool> closing_{false};
    std::mutex eventsMutex_;
    std::deque<Json> events_;
    size_t eventBytes_ = 0;
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
    std::string thread_, turn_, target_, turnTarget_, serverDefaultModel_;
    std::vector<std::string> targets_, modelIds_;
    std::vector<bool> reasoningModels_;
    std::wstring status_ = L"AI 未连接", pendingPrompt_, attachment_, valueTitle_, valueDraft_, turnTargetLabel_;
    std::string attachmentTarget_;
    bool busy_ = false, stopRequested_ = false, observedTool_ = false, dialogEnter_ = false, connectionError_ = false;
    size_t messageBytes_ = 0;
    Json pendingObservation_;
    ULONGLONG observeDeadline_ = 0, completionDeadline_ = 0, turnStarted_ = 0;
};
} // namespace serialctl
