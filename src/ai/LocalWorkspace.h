#pragma once
#include "WorkspaceActivity.h"
#include "json.hpp"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace serialctl::ai {

using Json = nlohmann::json;

struct LocalWorkspaceSnapshot {
    bool active = false;
    bool readOnly = false;
    std::wstring root;
    std::wstring canonicalRoot;
};

// Pure Win32/C++ helpers for Windows local workspace validation, browsing,
// AGENTS.md persistence, and safe sandboxed execution (Win7 SP1 compatible).
bool ResolveAndValidateLocalRoot(const std::wstring &root, std::wstring &canonicalOut, std::string &errorOut);
std::vector<std::wstring> EnumerateLocalDrives();
std::vector<std::wstring> EnumerateLocalSubdirectories(const std::wstring &dir, size_t maxEntries = 300);

bool ReadLocalAgentsMd(const std::wstring &root, bool &existsOut, std::string &contentOut, std::string &errorOut);
bool ReplaceLocalAgentsMd(const std::wstring &root, const std::string &expected, bool expectedExists,
                          const std::string &replacement, bool replacementExists, std::string &errorOut);

class LocalWorkspaceService {
  public:
    using ExecObserver = std::function<void(const ExecProgressEvent &)>;
    using FileChangeObserver = std::function<void(const FileChangeRecord &)>;

    LocalWorkspaceService();
    ~LocalWorkspaceService();

    // Registers an optional observer for live workspace.exec output and file mutations.
    std::uint64_t AddObserver(ExecObserver onExec, FileChangeObserver onFileChange);
    void RemoveObserver(std::uint64_t token);

    // Enables/updates or disables the active Windows local workspace sandbox root.
    // Returns false with errorOut if active==true and root cannot be resolved/opened.
    bool Configure(bool active, const std::wstring &root, bool readOnly, std::string &errorOut);
    LocalWorkspaceSnapshot Snapshot() const;

    // Signals any running workspace.exec (and long searches) to abort promptly.
    void CancelAllExecs();

    // Dispatches built-in workspace.* tool calls inside the active local root.
    // Never requires external git/rg/python/bash installations.
    Json ExecuteTool(const std::string &toolName, const Json &arguments,
                     const std::atomic<bool> *externalCancel = nullptr);

  private:
    struct ObserverEntry {
        std::uint64_t id = 0;
        ExecObserver onExec;
        FileChangeObserver onFileChange;
    };

    void EmitExecEvent(const ExecProgressEvent &event) const;
    void EmitFileChange(const FileChangeRecord &change) const;

    Json ToolInfo(const LocalWorkspaceSnapshot &snap) const;
    Json ToolList(const LocalWorkspaceSnapshot &snap, const Json &args) const;
    Json ToolSearchText(const LocalWorkspaceSnapshot &snap, const Json &args,
                        const std::atomic<bool> *externalCancel) const;
    Json ToolReadFile(const LocalWorkspaceSnapshot &snap, const Json &args) const;
    Json ToolWriteFile(const LocalWorkspaceSnapshot &snap, const Json &args) const;
    Json ToolEditFile(const LocalWorkspaceSnapshot &snap, const Json &args) const;
    Json ToolMove(const LocalWorkspaceSnapshot &snap, const Json &args) const;
    Json ToolDelete(const LocalWorkspaceSnapshot &snap, const Json &args) const;
    Json ToolExec(const LocalWorkspaceSnapshot &snap, const Json &args,
                  const std::atomic<bool> *externalCancel);

    mutable std::mutex mutex_;
    LocalWorkspaceSnapshot snapshot_;
    std::shared_ptr<std::atomic<bool>> execAbortEpoch_;
    mutable std::mutex observerMutex_;
    std::vector<ObserverEntry> observers_;
    mutable std::atomic<std::uint64_t> nextObserverId_{1};
    mutable std::atomic<std::uint64_t> nextExecSeq_{1};
    mutable std::atomic<std::uint64_t> nextChangeSeq_{1};
};

} // namespace serialctl::ai
