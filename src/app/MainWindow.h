#pragma once

#include "Connection.h"
#include "SessionLogger.h"
#include "SftpClient.h"
#include "SftpModel.h"
#include "TerminalModel.h"
#include "TerminalDecoder.h"
#include "PowerPane.h"
#include "ApiServer.h"
#include "SessionService.h"
#include "CmdLineEditor.h"
#include "ToolbarIcons.h"
#include <future>
#include <deque>

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

struct CommandItem {
    std::wstring name;
    std::vector<std::wstring> commands;
    std::uint32_t intervalMs = 500;

    CommandItem() = default;
    CommandItem(std::wstring itemName, std::wstring command)
        : name(std::move(itemName)), commands{std::move(command)} {}
};

class MainWindow {
public:
    bool Create(HINSTANCE instance, int showCommand);
    bool PreTranslate(MSG& message);
    HWND Handle() const { return window_; }

private:
    enum class SftpTransferDirection { Upload, Download };
    enum class SftpTransferState { Queued, Running, Completed, Failed, Canceled };
    enum class SftpMutationKind { CreateDirectory, Rename, Delete, ChangeMode };
    enum class SftpInputPurpose { Name, Mode, Path, Port };

    struct SftpTransferItem {
        std::uint64_t id = 0;
        std::uint64_t sessionId = 0;
        SftpTransferDirection direction = SftpTransferDirection::Upload;
        SftpTransferState state = SftpTransferState::Queued;
        std::wstring name;
        std::wstring localPath;
        std::wstring remotePath;
        std::uint64_t transferred = 0;
        std::uint64_t total = 0;
        std::uint64_t bytesPerSecond = 0;
        std::uint32_t remainingSeconds = 0;
        int percent = -1;
        std::wstring error;
        bool replaceExisting = false;
    };

    struct SftpBreadcrumbHit {
        RECT rect{};
        std::wstring path;
    };

    struct ReceivedItem {
        std::uint64_t sessionId = 0;
        Bytes data;
        UINT codePage = 0;
        std::string source; // empty for output, nonempty for accepted input
        LONGLONG arrival = 0;
    };
    struct SessionState {
        std::uint64_t id = 0;
        int mode = -1;
        std::wstring name;
        std::shared_ptr<IConnection> connection;
        std::unique_ptr<SessionLogger> logger;
        TerminalModel terminal;
        int terminalScrollOffset = 0;
        int terminalHorizontalOffset = 0;
        bool terminalHasNewOutput = false;
        std::wstring terminalTitle;
        int lineEndingIndex = 0;
        UINT codePage = CP_UTF8;
        TerminalDecoder decoder;
        CmdLineEditor cmdEditor;
        bool rawTrace = false;
        LONGLONG pendingPaintArrival = 0;
        std::wstring host;
        std::wstring username;
        std::wstring password;
        std::uint16_t port = 0;
        bool sftpFollowTerminal = true;
        std::wstring terminalDirectory;
        bool terminalDirectoryFromOsc = false;
        std::wstring sftpDirectory = L".";
        std::wstring sftpHomeDirectory;
        std::wstring pendingSftpDirectory;
        std::vector<SftpEntry> sftpEntries;
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
    static void PrepareDialogEditField(HWND dialog, HWND edit, MainWindow* self);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

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
    void RefreshSerialPorts(HWND dialog);
    void DiscoverSharedSerialPorts(HWND dialog);
    bool ReadConnectionDialog(HWND dialog, std::wstring& error);
    void ConnectFromDialog();
    void CompleteConnection(bool success, const std::wstring& error);
    void Disconnect();
    void DisconnectAll();
    void SwitchSession(size_t index);
    SessionState* FindSession(std::uint64_t id);
    bool SendBytesToActive(const Bytes& data, bool localEcho, const std::wstring& echoedText = {});
    void SendTerminalCharacter(wchar_t character);
    void SendTerminalKey(WPARAM key, bool shift, bool control, bool alt);
    void PasteToTerminal();
    void PaintTerminal(HDC dc);
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
    void StopCommandSequence(bool showStatus);
    void PostData(std::uint64_t sessionId, const Bytes& data);
    void PostStatus(std::uint64_t sessionId, const std::wstring& text, bool isError);
    void AppendData(std::uint64_t sessionId, const Bytes& data, UINT codePage = 0, LONGLONG arrival = 0);
    void QueueReceived(ReceivedItem);
    void AppendInput(const ReceivedItem&);
    std::wstring DecodeTerminalData(SessionState& session, const Bytes& data);
    void SyncSftpDirectoryFromTerminal(SessionState& session);
    void AppendStatus(const std::wstring& text, bool isError);
    void SaveCurrentLog();
    void SaveNetworkDiagnostics();
    void ShowTerminalContextMenu(POINT screenPoint);
    UINT SelectedCodePage() const;
    std::string EncodeTerminalText(const std::wstring&);
    std::wstring SelectedLineEnding() const;
    void SetConnectedUi(bool connected);
    void RefreshConnectionList();

    void LoadUiState();
    void SaveUiState() const;
    void SetRightPanelCollapsed(bool collapsed);

    void ShowSftpPanel(bool show);
    void RefreshSftp(const std::wstring& directory = {});
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
    int selectedMode_ = -1;
    int lineEndingIndex_ = 0;
    UINT selectedCodePage_ = CP_UTF8;
    bool localEchoEnabled_ = false;
    bool timestampEnabled_ = true;

    std::vector<HWND> toolbarButtons_;
    ApiServer apiServer_;
    std::vector<std::uint64_t> connectionListIds_;
    SessionService sessionService_;
    PowerService powerService_;
    PowerPane powerPane_;
    bool powerVisible_ = false;
    bool powerPageOpened_ = false;
    HWND cmdButton_ = nullptr, powerButton_ = nullptr;
    ToolbarIcons toolbarIcons_;
    HWND themeButton_ = nullptr;
    HWND disconnectButton_ = nullptr;
    HWND connectionHeader_ = nullptr;
    HWND connectionList_ = nullptr;
    HWND disconnectAllButton_ = nullptr;
    bool windowResizing_ = false;
    bool disconnectAllPending_ = false;
    bool disconnectAllInProgress_ = false;
    HWND terminal_ = nullptr;
    HWND status_ = nullptr;
    HWND statusTip_ = nullptr;
    std::wstring statusText_;
    int statusHeight_ = 0;
    HWND commandHeader_ = nullptr;
    HWND sftpTabButton_ = nullptr;
    HWND rightPanelToggleButton_ = nullptr;
    HWND commandList_ = nullptr;
    HWND saveCommandsButton_ = nullptr;
    bool commandsDirty_ = false;
    bool commandsFileProtected_ = false;
    HWND importButton_ = nullptr;
    HWND exportButton_ = nullptr;
    HWND addCommandButton_ = nullptr;
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
    std::uint64_t nextSessionId_ = 1;
    std::wstring activeConnectionName_;
    std::vector<CommandItem> commands_;
    int draggingCommandIndex_ = -1;
    int hoveredCommandIndex_ = -1;
    int hoveredCommandAction_ = 0;
    int runningCommandIndex_ = -1;
    size_t runningCommandStep_ = 0;
    std::uint64_t runningCommandSessionId_ = 0;
    UINT runningCommandCodePage_ = CP_UTF8;
    std::wstring runningCommandLineEnding_;
    HWND scrollingList_ = nullptr;
    int scrollingThumbOffset_ = 0;
    bool terminalSelecting_ = false;
    bool terminalSelectionActive_ = false;
    size_t terminalSelectionAnchorLine_ = 0;
    int terminalSelectionAnchorColumn_ = 0;
    size_t terminalSelectionFocusLine_ = 0;
    int terminalSelectionFocusColumn_ = 0;
    bool terminalScrollbarDragging_ = false;
    int terminalScrollbarThumbOffset_ = 0;
    bool terminalHorizontalScrollbarDragging_ = false;
    int terminalHorizontalScrollbarThumbOffset_ = 0;
    int terminalWheelRemainder_ = 0;
    int terminalHorizontalWheelRemainder_ = 0;
    int terminalZoomWheelRemainder_ = 0;
    int terminalFontHeight_ = 16;
    int terminalCellWidth_ = 9;
    int terminalLineHeight_ = 19;
    int terminalVisibleRows_ = 24;
    int terminalVisibleColumns_ = 80;
    std::wstring commandDirectory_;
    int rightPanelWidth_ = 360;
    bool rightPanelCollapsed_ = false;
    bool rightPanelAutoCollapsed_ = false;
    bool rightPanelDragging_ = false;
    bool rightPanelAnimating_ = false;
    int rightPanelAnimatedWidth_ = -1, rightPanelAnimationFrom_ = 0, rightPanelAnimationTo_ = 0;
    ULONGLONG rightPanelAnimationAt_ = 0;
    bool sftpPanelVisible_ = false;
    bool sftpBusy_ = false;
    std::wstring sftpDirectory_ = L".";
    std::vector<SftpEntry> sftpEntries_;
    SftpSortColumn sftpSortColumn_ = SftpSortColumn::Name;
    bool sftpSortAscending_ = true;
    std::vector<SftpBreadcrumbHit> sftpBreadcrumbHits_;
    int sftpNameWidth_ = 180;
    int sftpColumnDragging_ = 0;
    int sftpColumnDragX_ = 0;
    int sftpColumnDragWidth_ = 0;
    int hoveredSftpBreadcrumb_ = -1;
    std::thread sftpThread_;
    std::atomic_bool sftpOperationCancel_{false};
    std::uint64_t sftpOperationSessionId_ = 0;
    std::vector<SftpTransferItem> sftpTransfers_;
    std::thread sftpTransferThread_;
    std::atomic_bool sftpTransferCancel_{false};
    std::uint64_t nextSftpTransferId_ = 1;
    std::uint64_t activeSftpTransferId_ = 0;
    bool sftpTransferExpanded_ = false;
    std::uint64_t sftpRefreshSessionId_ = 0;
    std::wstring sftpRefreshDirectory_;
    bool statusIsError_ = false;

    std::thread connectionThread_;
    std::atomic_bool connectionCancel_{false};
    std::unique_ptr<SessionState> pendingSession_;
    std::vector<ReceivedItem> pendingConnectionData_;
    size_t pendingConnectionBytes_ = 0;
    std::atomic<bool> closing_{false};
    std::mutex receivedMutex_;
    std::deque<ReceivedItem> received_;
    std::map<std::uint64_t, size_t> receivedBytes_;
    bool measurePaint_ = false;
    std::uint64_t paintSample_ = 0;
    std::set<std::uint64_t> receiveOverflow_;
    bool receivePosted_ = false;
    std::set<std::uint64_t> loggerErrorsShown_;

    std::thread discoveryThread_;
    std::atomic_bool discoveryCancel_{false};
    std::atomic_bool discoveryFinished_{true};
    unsigned discoveryGeneration_ = 0;
    HWND discoveryDialog_ = nullptr;
    std::uint16_t discoveryExplicitPort_ = 0;
    int pendingMode_ = 0;
    SerialSettings pendingSerial_;
    std::wstring pendingHost_;
    std::wstring pendingUsername_;
    std::wstring pendingPassword_;
    std::uint16_t pendingPort_ = 0;
    std::wstring pendingRemoteSerial_;
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
};

} // namespace serialctl
