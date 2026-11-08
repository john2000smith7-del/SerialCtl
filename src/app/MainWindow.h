#pragma once
#include "BootInterrupt.h"

#include "AiPane.h"
#include "AiPetPane.h"
#include "ApiServer.h"
#include "CmdLineEditor.h"
#include "Connection.h"
#include "PowerPane.h"
#include "SerialPorts.h"
#include "SessionLogger.h"
#include "SessionService.h"
#include "SftpClient.h"
#include "SftpController.h"
#include "SftpModel.h"
#include "TerminalDecoder.h"
#include "TerminalModel.h"
#include "TerminalViewport.h"
#include "PaintBuffer.h"
#include "BackgroundCleanup.h"
#include "CommandMacroController.h"
#include "CommandNotebook.h"
#include "AiDeviceBridge.h"
#include "UiSettingsWriter.h"
#include "SshConnectionHistory.h"
#include "XferPane.h"
#include "TftpServer.h"
#include "TftpPane.h"
#include "ToolbarIcons.h"
#include "SessionReceiveDispatcher.h"
#include "McpIpc.h"
#include "McpHttpServer.h"
#include "McpDispatcher.h"
#include "McpService.h"
#include <deque>
#include <functional>
#include <future>

#include <atomic>
#include <cstdint>
#include <memory>
#include <map>
#include <set>
#include <mutex>
#include <shellapi.h>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace serialctl {

class UiLayoutBatch;

class MainWindow {
public:
    ~MainWindow();
    bool Create(HINSTANCE instance, int showCommand);
    void StartStartupUpdate();
    bool PreTranslate(MSG& message);
    HWND Handle() const { return window_; }
    bool Closing() const { return closing_.load(); }

private:
#ifdef SERIALCTL_AI_TESTING
    friend struct AiDeviceTestAccess;
    std::function<ULONGLONG()> commandClock_; // Deterministic input timing in native UI tests only.
#endif
    enum class SessionFocus { Terminal, Preserve, AiPreview, PowerPreview };
    using SftpTransferDirection = SftpController::Direction;
    using SftpTransferState = SftpController::TransferState;
    using SftpMutationKind = SftpController::Mutation;
    enum class SftpInputPurpose { Name, Mode, Path, Port };

    struct SettingsDialogState {
        MainWindow* owner = nullptr;
        std::uint64_t sessionId = 0;
        int initialEncoding = 0;
        wchar_t exportTip[2048]{};
        bool mcpEnabled = false;
        bool mcpAutoStart = false;
        uint16_t mcpPort = 7080;
    };

    struct LogExportJob {
        std::atomic<bool> cancel{false}, done{false}, copying{false};
        std::atomic<std::uint64_t> copied{0}, total{0};
        std::wstring source, destination, error;
        bool success = false;
    };
    using LogExportTask = std::function<bool(std::wstring &, const std::atomic<bool> *,
                                            const SessionLogger::ExportProgress &)>;

    using SftpTransferItem = SftpController::Transfer;

    struct SftpBreadcrumbHit {
        RECT rect{};
        std::wstring path;
    };

    using ReceivedItem = serialctl::ReceivedItem;
    struct SessionState : AiDeviceBridge::State, TerminalViewport::State, SftpController::Session {
        bool disconnected = false, disconnecting = false, closeAfterStop = false;
        std::shared_ptr<std::atomic_bool> cleanupDone;
        std::wstring disconnectCommandOutcome;
        SerialSettings serialSettings;
        std::string remoteInstance;
        std::wstring lastStatus;
        bool lastStatusIsError = false, unreadStatus = false;
        SYSTEMTIME statusTime{};
        std::uint64_t id = 0;
        int mode = -1;
        std::wstring name;
        std::shared_ptr<IConnection> connection;
        std::shared_ptr<SessionLogger> logger;
        int lineEndingIndex = 0;
        UINT codePage = CP_UTF8;
        bool rawTrace = false;
        bool saveSshHistory = false;
        bool rememberSshPassword = false;
        std::wstring employeeNumber;
        std::wstring host;
        std::wstring remoteCom;
        bool udpRaw = false, udpReceived = false;
        std::wstring username;
        std::wstring password;
        std::uint16_t port = 0;

    };
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TerminalSubclassProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR referenceData);
    static LRESULT CALLBACK ButtonSubclassProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR referenceData);
    static LRESULT CALLBACK DialogControlSubclassProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR referenceData);
    static LRESULT CALLBACK DialogFieldFrameSubclassProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR referenceData);
    static LRESULT CALLBACK CommandListSubclassProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR referenceData);
    static LRESULT CALLBACK CommandDragGhostProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK OverlayListSubclassProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR referenceData);
    static LRESULT CALLBACK SftpListSubclassProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR referenceData);
    static LRESULT CALLBACK SftpPathSubclassProc(
        HWND window, UINT message, WPARAM wParam, LPARAM lParam,
        UINT_PTR subclassId, DWORD_PTR referenceData);
    static INT_PTR CALLBACK ConnectionDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
    static INT_PTR CALLBACK CommandDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
    static INT_PTR CALLBACK SftpInputDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
    static INT_PTR CALLBACK SettingsDialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam);
    static void PrepareDialogEditField(HWND dialog, HWND edit, MainWindow* self);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    static INT_PTR CALLBACK UpdateDialogProc(HWND, UINT, WPARAM, LPARAM);
    void StopStartupUpdate();
    HWND updateDialog_ = nullptr;
    bool updateRequired_ = false, updateConfirming_ = true;
    std::string updateApprovedVersion_, updatePromptVersion_;
    std::atomic<std::uint64_t> updateReceived_{0}, updateTotal_{0};
    std::atomic<int> updatePhase_{0};
    bool CanInstallStartupUpdate(bool confirmed = false) const;

    Json ApiResources();
    Json ApiRequest(const std::string&,const std::string&,const Json&);
    Json ExecuteApiRequest(const std::string&,const std::string&,const Json&);
    void CreateControls();
    void LayoutControls(int width, int height);
    void PaintWindow(HDC dc);
    void DrawOwnerItem(const DRAWITEMSTRUCT& item);
    void ApplyTheme();
    void OpenConnectionDialog(int mode);
    void ConfigureConnectionDialog(HWND dialog);
    bool DeleteSshHistoryEntry(HWND dialog, size_t historyIndex);
    void RefreshConnectionHostIdentity(HWND dialog);
    void RefreshSerialPorts(HWND dialog);
    void CompleteSerialPortScan(HWND dialog);
    void DiscoverSharedSerialPorts(HWND dialog);
    bool ReadConnectionDialog(HWND dialog, std::wstring& error);
    void ConnectFromDialog();
    void CompleteConnection(bool success, const std::wstring& error);
    void Disconnect();
    void BeginDisconnect(SessionState&);
    void CloseSession(std::uint64_t);
    void PollSessionCleanup();
    void Reconnect();
    void RefreshConnectionActions();
    std::uint64_t pendingReconnectId_ = 0;
    void DisconnectAll();
    void SwitchSession(size_t index, SessionFocus focus = SessionFocus::Terminal);
    void ShowPowerPage();
    void ShowAiPage(bool connect = true);
    Json AiDeviceRequest(const std::string&, const Json&);
    SessionState* FindSession(std::uint64_t id);
    bool SendBytesToActive(const Bytes& data, bool localEcho, const std::wstring& echoedText = {}, std::function<bool()> allowed = {});
    void SendTerminalCharacter(wchar_t character);
    void SendTerminalKey(WPARAM key, bool shift, bool control, bool alt);
    void PasteToTerminal();
    void PaintTerminal(HDC dc);
    TerminalViewport::Options TerminalViewportOptions() const;
    void UpdateTerminalDimensions();
    void ScrollTerminal(int lines);
    void ScrollTerminalHorizontal(int columns);
    void ScrollTerminalToBottom();
    void ZoomTerminalFont(int steps);
    bool TerminalPointToCell(POINT point, size_t& line, int& column) const;
    void UpdateTerminalSelection(size_t line, int column, bool extend);
    void ClearTerminalSelection();
    bool HasTerminalSelection() const;
    std::wstring TerminalSelectionText(bool selectAll) const;
    void CopyTerminalSelection(bool selectAll);
    void SendCommand(size_t index);
    void SendNextCommandStep();
    using CommandSequenceEnd = CommandMacroController::End;
    void StopCommandSequence(CommandSequenceEnd end, const std::wstring& detail = {});
    void PostData(std::uint64_t sessionId, const Bytes& data, UINT codePage);
    void PostStatus(std::uint64_t sessionId, const std::wstring& text, bool isError);
    void AppendData(std::uint64_t sessionId, const Bytes &data, UINT codePage = 0, LONGLONG arrival = 0);
    void QueueReceived(ReceivedItem);
    void AppendInput(const ReceivedItem &);
    std::wstring DecodeTerminalData(SessionState& session, const Bytes& data);
    void SyncSftpDirectoryFromTerminal(SessionState& session);
    void AppendStatus(const std::wstring& text, bool isError);
    void SaveCurrentLog(HWND owner = nullptr);
    void StartLogExport(const std::wstring &, const std::wstring &, std::uint64_t, LogExportTask);
    void PollLogExport();
    void CancelLogExport();
    void StopLogExport();
    void JoinLogExport();
    void RequestShutdown();
    void WaitForShutdown();
    void FinishShutdown();
    void DrainShutdownMessages();
    void FinishPendingConnection();
    void SaveNetworkDiagnostics(HWND owner = nullptr);
    void ShowSettings();
    void ExecuteTerminalAction(UINT command, HWND owner = nullptr);
    UINT SelectedCodePage() const;
    std::string EncodeTerminalText(const std::wstring &);
    std::wstring SelectedLineEnding() const;
    void SetConnectedUi(bool connected);
    void RefreshConnectionList();

    void LoadUiState();
    void SaveUiState();
    void LoadTftpState();
    void SetRightPanelCollapsed(bool collapsed);
    void SetLeftPanelCollapsed(bool collapsed);
    int LeftWidth() const;
    int ExpandedLeftWidth() const;
    std::wstring BootProfileKey(const SessionState&) const;
    bool SidebarCollapsed() const;
    int SidebarWidth() const;
    void SyncAiTerminal();
    void RefreshPowerTargets();
    void SyncPowerTerminal();
    bool LiveMonitorPage() const { return aiVisible_ || powerVisible_; }

    void ShowSftpPanel(bool show,bool preserveXfer=false);
    void ShowXferPanel();
    void ShowTftpPanel(bool show=true);
    std::vector<TftpSshSessionInfo> GetTftpSshSessions() const;
    XferPane::Target XferTarget();
    void QueueSidebarVisibility(UiLayoutBatch& batch, int contentWidth);
    void RefreshSftp(const std::wstring& directory = {});
    static SftpController::Endpoint SftpEndpoint(const SessionState&);
    void EditSftpPath();
    void QueryTerminalDirectory(bool installHook);
    void SftpColumnWidths(int width, int& name, int& size, int& modified) const;
    static LRESULT CALLBACK SftpHeaderSubclassProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    void NavigateSftp(size_t index);
    void NavigateSftpBreadcrumb(POINT point);
    void SetSftpSort(SftpSortColumn column);
    std::vector<size_t> SelectedSftpIndices() const;
    void ShowSftpContextMenu(POINT screenPoint);
    void ShowSftpTransferContextMenu(POINT screenPoint);
    void HandleSftpDrop(HDROP drop);
    void UploadSftp();
    void DownloadSftp();
    bool StartSftpUpload(const std::wstring& localPath,
        const std::wstring& remoteDirectory = {});
    void StartSftpDownload(const std::wstring& remotePath, const std::wstring& localPath,
        std::uint64_t expectedSize, bool replaceExisting);
    void StartNextSftpTransfer();
    void CancelSftpTransfer(std::uint64_t transferId);
    void RetrySftpTransfer(std::uint64_t transferId);
    void ClearFinishedSftpTransfers();
    void RefreshSftpTransferList();
    void HandleSftpProgress(LPARAM value);
    void CreateSftpDirectory();
    void RenameSelectedSftpEntry();
    void DeleteSelectedSftpEntries();
    void ChangeSelectedSftpMode();
    void CopySelectedSftpPaths();
    void EnterSelectedSftpDirectoryInTerminal();
    void StartSftpMutation(SftpMutationKind kind, const std::vector<std::wstring>& paths,
        const std::wstring& value = {}, const std::vector<bool>& directories = {});
    bool PromptSftpValue(const std::wstring& title, const std::wstring& label,
        const std::wstring& initialValue, SftpInputPurpose purpose, std::wstring& value, HWND owner = nullptr);
    void HandleSftpMessage(LPARAM value);
    void SetSftpBusy(bool busy);
    void RefreshSftpList();

    void LoadCommands();
    bool SaveCommands();
    void MarkCommandsDirty();
    bool LoadCommandsFromFile(const std::wstring& path, std::wstring& error);
    bool SaveCommandsToFile(const std::wstring& path, std::wstring& error) const;
    void RefreshCommandList();
    void UpdateCommandActions();
    void EditCommand(size_t index);
    void MoveCommand(size_t from, size_t to);
    void UpdateCommandDrag(POINT point, bool edgeScroll);
    void EndCommandDrag(bool commit);
    void ImportCommandFile(const std::wstring& path);
    void ImportCommands();
    void ExportCommands();
    void AddCommand();
    void DeleteSelectedCommand();
    void CreateCommandDialogStep(HWND dialog, const std::wstring& text);
    void ResetCommandDialogSteps(HWND dialog, const std::vector<std::wstring>& commands);
    void LayoutCommandDialog(HWND dialog);
    void UpdateCommandDialogValidation(HWND dialog);
    std::wstring DefaultCommandsPath() const;
    std::wstring InitialCommandDirectory();
    void RememberCommandDirectory(const std::wstring& selectedPath);

    HWND CreateChild(const wchar_t* type, const wchar_t* text, DWORD style, int id);
    static std::wstring ControlText(HWND control);
    static void SetControlText(HWND control, const std::wstring& text);

    HWND window_ = nullptr;
    HINSTANCE instance_ = nullptr;
    HFONT uiFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HFONT titleFont_ = nullptr;
    HFONT terminalFont_ = nullptr;
    HBRUSH windowBrush_ = nullptr;
    HBRUSH panelBrush_ = nullptr;
    HBRUSH terminalBrush_ = nullptr;
    bool darkMode_ = false;
    bool compactNavigation_ = false;
    HWND navigationTooltip_ = nullptr;
    std::wstring navigationTipText_;
    int selectedMode_ = -1;
    int lineEndingIndex_ = 0;
    UINT selectedCodePage_ = CP_UTF8;
    bool localEchoEnabled_ = false;
    bool timestampEnabled_ = true;
    UiFieldStyle dialogFieldStyle_{&darkMode_, nullptr, 96};

    std::vector<HWND> toolbarButtons_;
    ApiServer apiServer_;
    std::vector<std::uint64_t> connectionListIds_;
    SessionService sessionService_;
    PowerService powerService_;
    AiDeviceBridge aiDeviceBridge_{powerService_, sessionService_};
    PowerPane powerPane_;
    AiPane aiPane_;
    AiPetPane aiPet_;
    pet::Settings petSettings_;
    std::wstring uiSettingsPath_;
    UiSettingsWriter uiSettingsWriter_;
    bool aiVisible_ = false, aiPageOpened_ = false;
    bool aiMonitorCollapsed_ = false;
    int aiMonitorWidth_ = 360;
    int aiMonitorRatio_ = 500;
    bool aiMonitorWrap_ = false;
    HWND aiWrapButton_ = nullptr;
    HWND aiTerminalHeader_ = nullptr;
    bool aiRemoteConnect_ = false;
    HWND aiButton_ = nullptr;
    bool powerVisible_ = false;
    bool powerPageOpened_ = false;
    bool powerMonitorCollapsed_ = false, powerMonitorWrap_ = false;
    int powerMonitorRatio_ = 500;
    HWND powerMonitorTarget_ = nullptr;
    std::string powerMonitorSession_;
    std::vector<std::string> powerMonitorIds_;
    std::vector<std::wstring> powerMonitorLabels_;
    HWND cmdButton_ = nullptr, powerButton_ = nullptr;
    ToolbarIcons toolbarIcons_;
    HWND themeButton_ = nullptr;
    HWND settingsButton_ = nullptr;
    HWND disconnectButton_ = nullptr;
    HWND reconnectButton_ = nullptr;
    HWND connectionHeader_ = nullptr;
    HWND connectionList_ = nullptr;
    HWND disconnectAllButton_ = nullptr;
    bool windowResizing_ = false;
    bool disconnectAllPending_ = false;
    std::string powerDisconnectActionId_;
    std::wstring powerAlert_;
    bool disconnectAllInProgress_ = false;
    HWND terminal_ = nullptr;
    HWND status_ = nullptr;
    HWND statusTip_ = nullptr;
    std::wstring statusText_;
    int statusHeight_ = 0;
    HWND commandHeader_ = nullptr;
    HWND sftpTabButton_ = nullptr;
    HWND xferTabButton_ = nullptr;
    HWND tftpTabButton_ = nullptr;
    HWND rightPanelToggleButton_ = nullptr;
    CommandNotebook notebook_;
    HWND commandHint_ = nullptr;
    HWND commandList_ = nullptr;
    bool notebookRepeating_ = false;
    std::uint64_t notebookRound_ = 0;
    std::shared_ptr<std::atomic_bool> commandSendPermit_;
    void RunNotebook(std::vector<std::wstring> commands);
    std::wstring NotebookPath() const;
    std::wstring lastNotebookPlan_;
    ULONGLONG lastNotebookAt_ = 0;
    std::uint64_t lastNotebookSession_ = 0;
    HWND saveCommandsButton_ = nullptr;
    HWND importButton_ = nullptr;
    HWND exportButton_ = nullptr;
    HWND addCommandButton_ = nullptr;
    bool addCommandCompact_ = false;
    HWND deleteCommandButton_ = nullptr;
    HWND sftpPath_ = nullptr;
    HWND sftpNameHeader_ = nullptr;
    HWND sftpSizeHeader_ = nullptr;
    HWND sftpModifiedHeader_ = nullptr;
    HWND sftpList_ = nullptr;
    HWND sftpTooltip_ = nullptr;
    std::wstring sftpHoverText_;
    HWND sftpTransferToggleButton_ = nullptr;
    HWND sftpTransferList_ = nullptr;
    HWND sftpClearTransfersButton_ = nullptr;
    HWND sftpUpButton_ = nullptr;
    HWND sftpRefreshButton_ = nullptr;
    HWND sftpUploadButton_ = nullptr;
    HWND sftpDownloadButton_ = nullptr;

    std::vector<std::unique_ptr<SessionState>> sessions_;
    SessionState* activeSession_ = nullptr;
    IConnection* connection_ = nullptr;
    SessionLogger* logger_ = nullptr;
    std::shared_ptr<LogExportJob> logExportJob_;
    std::thread logExportThread_;
    std::wstring logExportNotice_;
    std::wstring logExportMainStatus_;
    std::uint64_t nextSessionId_ = 1;
    std::wstring activeConnectionName_;
    CommandMacroController macro_;
    int draggingCommandIndex_ = -1;
    int commandDropIndex_ = -1;
    int pendingCommandDragIndex_ = -1;
    int commandInsertIndex_ = -1;
    bool commandDragInside_ = true;
    POINT commandDragOrigin_{};
    POINT commandDragGrab_{};
    HWND commandDragGhost_ = nullptr;
    int hoveredCommandIndex_ = -1;
    int hoveredCommandAction_ = 0;
    int pressedCommandIndex_ = -1, pressedCommandAction_ = 0;
    int armedCommandIndex_ = -1, armedCommandAction_ = 0;
    HWND scrollingList_ = nullptr;
    bool connectionListKeyboardInput_ = false;
    int scrollingThumbOffset_ = 0;
    int terminalFontHeight_ = 16;
    std::wstring commandDirectory_;
    int rightPanelWidth_ = 360;
    bool rightPanelCollapsed_ = false;
    std::string pendingPowerMonitorSession_;
    std::map<std::wstring,std::pair<std::string,unsigned>> bootPresets_;
    bool leftPanelCollapsed_ = false, leftPanelAnimating_ = false;
    int leftPanelAnimatedWidth_ = -1, leftPanelAnimationFrom_ = 0, leftPanelAnimationTo_ = 0;
    ULONGLONG leftPanelAnimationAt_ = 0;
    bool rightPanelAutoCollapsed_ = false;
    bool rightPanelDragging_ = false, leftPanelDragging_ = false;
    int leftPanelWidth_ = 190;
    std::wstring clipboardNotice_;
    void ShowClipboardNotice(const std::wstring&);
    bool rightPanelAnimating_ = false;
    int rightPanelAnimatedWidth_ = -1, rightPanelAnimationFrom_ = 0, rightPanelAnimationTo_ = 0;
    ULONGLONG rightPanelAnimationAt_ = 0;
    SftpController sftp_;
    xfer::TransferController xfer_;
    XferPane xferPane_;
    bool xferPanelVisible_ = false;
    bool sftpPanelVisible_ = false;
    std::shared_ptr<TftpServer> tftpServer_;
    TftpPane tftpPane_;
    bool tftpPanelVisible_ = false;
    std::vector<SftpBreadcrumbHit> sftpBreadcrumbHits_;
    int sftpNameWidth_ = 180;
    int sftpColumnDragging_ = 0;
    int sftpColumnDragX_ = 0;
    int sftpColumnDragWidth_ = 0;
    int hoveredSftpBreadcrumb_ = -1;
    bool sftpTransferExpanded_ = false;
    bool statusIsError_ = false;

    std::thread connectionThread_;
    BackgroundCleanup connectionCleanup_;
    PaintBuffer windowPaintBuffer_;
    TerminalViewport terminalViewport_;
    std::atomic_bool connectionCancel_{false};
    bool cancelledConnectionPending_ = false;
    bool connectionResultReady_ = false, connectionResultSuccess_ = false;
    std::wstring connectionResultError_;
    std::unique_ptr<SessionState> pendingSession_;
    std::vector<ReceivedItem> pendingConnectionData_;
    size_t pendingConnectionBytes_ = 0;
    std::atomic<bool> closing_{false};
    std::thread shutdownThread_;
    bool shutdownComplete_ = false;
    std::thread updateThread_;
    std::atomic<bool> updateCancel_{false};
    bool startupUpdateDeferred_ = false;
    SessionReceiveDispatcher receiveDispatcher_;
    std::deque<ReceivedItem> &received_ = receiveDispatcher_.DirectItems();
    std::mutex &receivedMutex_ = receiveDispatcher_.DirectMutex();
    unsigned &receiveFairness_ = receiveDispatcher_.DirectFairness();
    bool measurePaint_ = false;
    bool receivePosted_ = false;
    bool receiveContinuation_ = false; // UI thread: continue at the next paint opportunity, with no timer delay.
    std::set<std::uint64_t> loggerErrorsShown_;

    std::thread discoveryThread_;
    std::atomic_bool discoveryCancel_{false};
    std::atomic_bool discoveryFinished_{true};
    unsigned discoveryGeneration_ = 0;
    HWND discoveryDialog_ = nullptr;
    std::uint16_t discoveryExplicitPort_ = 0;
    int pendingMode_ = 0;
    bool pendingHostKeyReverify_ = false;
    bool pendingSaveSshHistory_ = false;
    bool pendingRememberSshPassword_ = false;
    std::wstring loadedSshCredentialTarget_;
    std::wstring pendingEmployeeNumber_;
    Json sshHistory_ = Json::array();
    bool initializingConnectionDialog_ = false;
    SerialSettings pendingSerial_;
    std::wstring pendingHost_;
    std::wstring pendingUsername_;
    std::wstring pendingPassword_;
    std::uint16_t pendingPort_ = 0;
    std::wstring pendingRemoteSerial_, pendingRemoteLabel_;
    std::vector<SerialPortInfo> serialPorts_;
    struct SerialPortScan {
        std::atomic<bool> done{false};
        std::vector<SerialPortInfo> ports;
        std::wstring error;
    };
    std::shared_ptr<SerialPortScan> serialPortScan_;
    std::thread serialPortScanThread_;
    std::string pendingRemoteInstance_;
    CommandItem pendingCommand_;
    std::wstring pendingSftpInputTitle_;
    std::wstring pendingSftpInputLabel_;
    std::wstring pendingSftpInputValue_;
    std::wstring pendingSftpInputError_;
    SftpInputPurpose pendingSftpInputPurpose_ = SftpInputPurpose::Name;
    int editingCommandIndex_ = -1;
    std::vector<HWND> commandDialogStepEdits_;
    std::vector<HWND> commandDialogRemoveButtons_;

    std::unique_ptr<mcp::McpIpcServer> mcpIpcServer_;
    std::unique_ptr<mcp::McpHttpServer> mcpEmbeddedServer_;
    std::unique_ptr<mcp::McpDispatcher> mcpDispatcher_;
    std::atomic<bool> mcpStopping_{false};
    bool mcpEnabled_ = false;
    bool mcpAutoStart_ = false;
    uint16_t mcpPort_ = 7080;
    std::string mcpBindIp_ = "0.0.0.0";

    void StartMcpIpc();
    void StopMcpIpc();
    std::unique_ptr<mcp::McpDispatcher> CreateMcpDispatcher(bool http);
    void StartEmbeddedMcpServer();
    void StopEmbeddedMcpServer();
    void ApplyMcpSettings(bool enabled, bool autoStart, uint16_t port);
    Json DispatchMcpToUi(const std::string& method, const Json& params, bool http);
    Json McpExecute(const std::string& method, const Json& params);
    SessionState* ResolveTargetSession(const Json& params, std::string& err);
    uint64_t mcpRequestSeq_ = 0;
};

} // namespace serialctl
