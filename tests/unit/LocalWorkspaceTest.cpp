#include "AiAttachment.h"
#include "LocalWorkspace.h"
#include "Workspace.h"
#include "WorkspaceInstructions.h"
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <windows.h>

using namespace serialctl::ai;

namespace {

void Require(bool condition, const char *reason) {
    if (!condition)
        throw std::runtime_error(reason);
}

std::wstring CreateTempTestDir(const std::wstring &tag) {
    wchar_t tmp[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    Require(n > 0 && n < MAX_PATH, "GetTempPathW failed");
    std::wstring dir = std::wstring(tmp, n) + L"serialctl-" + tag + L"-" + std::to_wstring(GetCurrentProcessId());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    Require(std::filesystem::create_directories(dir, ec), "Failed to create test directory");
    return dir;
}

bool TryCreateJunction(const std::wstring &linkPath, const std::wstring &targetPath) {
    // Uses built-in cmd.exe mklink /J (works without elevation on NTFS).
    std::wstring cmd = L"cmd.exe /d /c mklink /J \"" + linkPath + L"\" \"" + targetPath + L"\" >nul 2>nul";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        return false;
    WaitForSingleObject(pi.hProcess, 5000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

} // namespace

int main() {
    std::wstring rootDir;
    std::wstring outsideDir;
    std::string stagedDir;
    try {
        // 1. Pure path validation & binding serialization checks
        Require(ValidRemoteWorkspacePath("/home/linux/project") && !ValidRemoteWorkspacePath("D:\\proj"),
                "Remote path validator confused POSIX and Windows paths");
        Require(ValidLocalWorkspacePath("D:\\workspace\\bsp") && ValidLocalWorkspacePath("d:/workspace/bsp"),
                "Windows drive-letter paths must be accepted");
        Require(!ValidLocalWorkspacePath("relative\\path") && !ValidLocalWorkspacePath("\\\\server\\share\\p") &&
                    !ValidLocalWorkspacePath("//server/share") && !ValidLocalWorkspacePath("\\\\.\\C:\\Windows") &&
                    !ValidLocalWorkspacePath("\\\\?\\C:\\Windows") && !ValidLocalWorkspacePath("D:\\a\\..\\b") &&
                    !ValidLocalWorkspacePath("D:\\bad:name") && !ValidLocalWorkspacePath(std::string("D:\\a\0b", 6)),
                "Unsafe UNC/device/traversal/control Windows paths must be rejected");
        Require(NormalizeLocalWorkspacePathUtf8("d:/proj//sub/") == "D:\\proj\\sub",
                "Local path normalization failed");
        Require(NormalizeLocalWorkspacePathUtf8("c:/") == "C:\\", "Drive root normalization failed");
        Require(WorkspaceParent("D:\\proj\\sub") == "D:\\proj" && WorkspaceParent("D:\\proj") == "D:\\" &&
                    WorkspaceParent("D:\\") == "D:\\",
                "Windows WorkspaceParent traversal invalid");
        Require(WorkspaceParent("/srv/code/") == "/srv" && WorkspaceParent("/") == "/",
                "POSIX WorkspaceParent traversal invalid");
        Require(WorkspaceLeafLabel(L"D:\\proj\\sub\\") == L"sub" && WorkspaceLeafLabel(L"d:\\") == L"D:\\" &&
                    WorkspaceLeafLabel(L"/home/linux/") == L"linux" && WorkspaceLeafLabel(L"/") == L"/",
                "WorkspaceLeafLabel mismatch");

        const auto modernBinding = ParseStoredWorkspaceBinding(
            {{"workspaceIsLocal", true},
             {"localWorkspace", "d:/repo/demo"},
             {"remoteWorkspace", "/srv/demo"},
             {"remoteSshSessionId", "ssh-2"}});
        Require(modernBinding.hasExplicitMode && modernBinding.isLocal &&
                    modernBinding.localPath == "D:\\repo\\demo" &&
                    modernBinding.remotePath == "/srv/demo" &&
                    modernBinding.remoteSessionId == "ssh-2",
                "ParseStoredWorkspaceBinding lost decoupled fields");

        const auto legacyLocal = ParseStoredWorkspaceBinding({{"cwd", "e:/legacy/fw"}});
        Require(legacyLocal.hasExplicitMode && legacyLocal.isLocal && legacyLocal.localPath == "E:\\legacy\\fw",
                "Legacy Windows cwd binding failed to migrate cleanly");

        const auto legacyRemote = ParseStoredWorkspaceBinding({{"profile", "host:22/u/project:/srv/fw"}});
        Require(!legacyRemote.hasExplicitMode && !legacyRemote.isLocal && legacyRemote.remotePath == "/srv/fw",
                "Legacy remote binding should retain remote default");

        // 2. Native Win32 directory validation, enumeration & junction/symlink blocking
        rootDir = CreateTempTestDir(L"lw-root");
        outsideDir = CreateTempTestDir(L"lw-outside");
        {
            std::ofstream out(std::filesystem::path(outsideDir) / L"secret.txt");
            out << "TOP_SECRET_OUTSIDE\n";
        }
        std::filesystem::create_directories(std::filesystem::path(rootDir) / L"alpha");
        std::filesystem::create_directories(std::filesystem::path(rootDir) / L"beta");

        std::wstring canonicalRoot;
        std::wstring ignoredCanonical;
        std::string err;
        Require(ResolveAndValidateLocalRoot(rootDir, canonicalRoot, err) && !canonicalRoot.empty(),
                ("Valid local root rejected: " + err).c_str());
        Require(!ResolveAndValidateLocalRoot(rootDir + L"\\missing-subdir-xyz", ignoredCanonical, err),
                "Non-existent directory accepted");
        Require(!ResolveAndValidateLocalRoot(L"\\\\127.0.0.1\\C$", ignoredCanonical, err),
                "UNC root accepted");

        const auto drives = EnumerateLocalDrives();
        Require(!drives.empty(), "EnumerateLocalDrives returned zero drives on Windows");
        const auto children = EnumerateLocalSubdirectories(canonicalRoot);
        Require(children.size() == 2, "EnumerateLocalSubdirectories missed child folders");

        const std::wstring junctionPath = rootDir + L"\\escape_link";
        const bool junctionCreated = TryCreateJunction(junctionPath, outsideDir);
        if (junctionCreated) {
            std::wstring badRoot;
            Require(!ResolveAndValidateLocalRoot(junctionPath, badRoot, err),
                    "Junction root must be rejected as reparse point");
            const auto listedAfterJunction = EnumerateLocalSubdirectories(canonicalRoot);
            Require(listedAfterJunction.size() == 2, "EnumerateLocalSubdirectories exposed a reparse point");
        }

        // 3. AGENTS.md atomic CAS read / write / rollback
        bool exists = true;
        std::string content;
        Require(ReadLocalAgentsMd(canonicalRoot, exists, content, err) && !exists && content.empty(),
                "Absent AGENTS.md should report exists=false");
        const std::string v1 = MergeManagedBlock("", {"Use workspace.list before exec."});
        Require(ReplaceLocalAgentsMd(canonicalRoot, "", false, v1, true, err),
                ("Initial local AGENTS.md create failed: " + err).c_str());
        Require(!ReplaceLocalAgentsMd(canonicalRoot, "", false, "stale overwrite", true, err),
                "Stale CAS expectation overwrote newer AGENTS.md");
        Require(ReadLocalAgentsMd(canonicalRoot, exists, content, err) && exists && content == v1,
                "AGENTS.md content changed after rejected CAS update");
        Require(ReplaceLocalAgentsMd(canonicalRoot, v1, true, "", false, err),
                ("AGENTS.md rollback delete failed: " + err).c_str());
        Require(ReadLocalAgentsMd(canonicalRoot, exists, content, err) && !exists,
                "AGENTS.md still exists after rollback delete");

        // 4. LocalWorkspaceService: inactive gating + full 9 built-in tools
        LocalWorkspaceService svc;
        const auto inactiveInfo = svc.ExecuteTool("workspace.info", Json::object());
        Require(inactiveInfo["ok"] == true && inactiveInfo["active"] == false,
                "Inactive workspace.info must report active=false");
        const auto inactiveList = svc.ExecuteTool("workspace.list", Json::object());
        Require(inactiveList["ok"] == false && inactiveList["code"] == "LOCAL_WORKSPACE_NOT_ACTIVE",
                "Inactive workspace service must reject workspace.list");

        Require(svc.Configure(true, canonicalRoot, false, err), ("Configure failed: " + err).c_str());
        const auto info = svc.ExecuteTool("workspace.info", Json::object());
        Require(info["ok"] == true && info["active"] == true && info["readOnly"] == false &&
                    info["gitRequired"] == false && info["builtinTools"].size() == 9,
                "workspace.info mismatch");

        // Write, traversal/reserved rejection, binary rejection
        Require(svc.ExecuteTool("workspace.write_file", {{"path", "src/hello.c"},
                                                       {"content", "int main(void) {\n    // MAGIC_TOKEN_ALPHA\n    return 0;\n}\n"}})["ok"] == true,
                "workspace.write_file failed");
        Require(svc.ExecuteTool("workspace.write_file", {{"path", "..\\outside.txt"}, {"content", "x"}})["ok"] == false,
                "Parent traversal write accepted");
        Require(svc.ExecuteTool("workspace.write_file", {{"path", "C:\\Windows\\System32\\bad.txt"}, {"content", "x"}})["ok"] == false,
                "Absolute external path accepted");
        Require(svc.ExecuteTool("workspace.write_file", {{"path", "src/NUL.txt"}, {"content", "x"}})["ok"] == false,
                "DOS reserved device name accepted");
        Require(svc.ExecuteTool("workspace.write_file", {{"path", "src/bin.dat"}, {"content", std::string("A\0B", 3)}})["ok"] == false,
                "Binary NUL content accepted in write_file");
        if (junctionCreated) {
            Require(svc.ExecuteTool("workspace.read_file", {{"path", "escape_link/secret.txt"}})["ok"] == false,
                    "Junction escape succeeded in read_file");
            Require(svc.ExecuteTool("workspace.delete", {{"path", "escape_link"}, {"confirm", true}})["ok"] == false,
                    "Junction deletion should be blocked");
        }

        // Read bounded lines
        const auto readRes = svc.ExecuteTool("workspace.read_file", {{"path", "src/hello.c"}, {"startLine", 2}, {"maxLines", 2}});
        Require(readRes["ok"] == true && readRes["startLine"] == 2 && readRes["endLine"] == 3 &&
                    readRes["totalLines"] == 4 && readRes["content"].get<std::string>().find("MAGIC_TOKEN_ALPHA") != std::string::npos,
                "workspace.read_file line slicing failed");

        // List with depth + wildcard
        const auto listRes = svc.ExecuteTool("workspace.list", {{"path", "."}, {"depth", 3}, {"pattern", "*.c"}});
        Require(listRes["ok"] == true && listRes["entries"].size() == 1 &&
                    listRes["entries"][0]["path"] == "src/hello.c",
                "workspace.list depth/pattern mismatch");

        // Search text (substring & regex, zero rg/git required)
        const auto searchPlain = svc.ExecuteTool("workspace.search_text", {{"query", "magic_token_alpha"}});
        Require(searchPlain["ok"] == true && searchPlain["matches"].size() == 1 &&
                    searchPlain["matches"][0]["line"] == 2 && searchPlain["matches"][0]["path"] == "src/hello.c",
                "workspace.search_text case-insensitive match failed");
        const auto searchRegex = svc.ExecuteTool("workspace.search_text",
                                                 {{"query", "MAGIC_[A-Z]+_ALPHA"}, {"regex", true}, {"caseSensitive", true}});
        Require(searchRegex["ok"] == true && searchRegex["matches"].size() == 1,
                "workspace.search_text regex match failed");

        // Edit file (CRLF preservation + ambiguous rejection)
        Require(svc.ExecuteTool("workspace.write_file", {{"path", "src/crlf.txt"}, {"content", "line1\r\ndup\r\ndup\r\n"}})["ok"] == true,
                "CRLF fixture write failed");
        Require(svc.ExecuteTool("workspace.edit_file",
                                {{"path", "src/crlf.txt"}, {"oldText", "dup"}, {"newText", "one"}})["ok"] == false,
                "Ambiguous edit should fail when replaceAll=false");
        Require(svc.ExecuteTool("workspace.edit_file",
                                {{"path", "src/crlf.txt"}, {"oldText", "line1\ndup"}, {"newText", "line1\nfirst"}})["ok"] == true,
                "CRLF normalized edit_file failed");
        const auto afterEdit = svc.ExecuteTool("workspace.read_file", {{"path", "src/crlf.txt"}});
        Require(afterEdit["content"] == "line1\nfirst\ndup", "Edit result mismatch");

        // Move & delete (confirmation enforcement)
        Require(svc.ExecuteTool("workspace.move", {{"from", "src/crlf.txt"}, {"to", "moved/crlf.txt"}})["ok"] == true,
                "workspace.move failed");
        Require(svc.ExecuteTool("workspace.delete", {{"path", "moved/crlf.txt"}, {"confirm", false}})["code"] == "CONFIRMATION_REQUIRED",
                "workspace.delete succeeded without confirm=true");
        Require(svc.ExecuteTool("workspace.delete", {{"path", "moved/crlf.txt"}, {"confirm", true}})["ok"] == true,
                "workspace.delete with confirm=true failed");

        // Exec (cmd.exe, stdout, exitCode, timeout, cancelAll)
        const auto execOk = svc.ExecuteTool("workspace.exec", {{"command", "echo SERIALCTL_EXEC_OK"}});
        Require(execOk["ok"] == true && execOk["exitCode"] == 0 &&
                    execOk["stdout"].get<std::string>().find("SERIALCTL_EXEC_OK") != std::string::npos,
                "workspace.exec echo failed");
        const auto execFail = svc.ExecuteTool("workspace.exec", {{"command", "exit /b 7"}});
        Require(execFail["ok"] == true && execFail["exitCode"] == 7, "workspace.exec nonzero exit code mismatch");

        const auto execTimeout = svc.ExecuteTool("workspace.exec",
                                               {{"command", "ping -n 10 127.0.0.1 >nul"}, {"timeoutMs", 250}});
        Require(execTimeout["ok"] == false && execTimeout["timedOut"] == true,
                "workspace.exec timeout did not trigger");

        std::atomic<bool> extCancel{false};
        Json asyncExecResult;
        std::thread execThread([&] {
            asyncExecResult = svc.ExecuteTool("workspace.exec",
                                              {{"command", "ping -n 15 127.0.0.1 >nul"}, {"timeoutMs", 15000}},
                                              &extCancel);
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        const auto cancelStart = std::chrono::steady_clock::now();
        svc.CancelAllExecs();
        execThread.join();
        const auto cancelElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - cancelStart).count();
        Require(asyncExecResult["ok"] == false && asyncExecResult["cancelled"] == true && cancelElapsed < 3000,
                "CancelAllExecs did not abort workspace.exec promptly");

        // 4b. Activity observers (streaming exec output + file change records & diff)
        {
            std::mutex obsMu;
            std::vector<ExecProgressEvent> execEvents;
            std::vector<FileChangeRecord> fileChanges;
            const auto obsToken = svc.AddObserver(
                [&](const ExecProgressEvent &ev) {
                    std::lock_guard<std::mutex> lock(obsMu);
                    execEvents.push_back(ev);
                },
                [&](const FileChangeRecord &rec) {
                    std::lock_guard<std::mutex> lock(obsMu);
                    fileChanges.push_back(rec);
                });

            const auto streamExec = svc.ExecuteTool(
                "workspace.exec", {{"command", "echo STREAM_OBS_A& echo STREAM_OBS_B"}});
            Require(streamExec["ok"] == true && streamExec["exitCode"] == 0,
                    "Observer workspace.exec failed");
            Require(svc.ExecuteTool("workspace.write_file",
                                    {{"path", "src/obs_test.txt"}, {"content", "alpha\nbeta\n"}})["ok"] == true,
                    "Observer write_file failed");
            Require(svc.ExecuteTool("workspace.edit_file",
                                    {{"path", "src/obs_test.txt"}, {"oldText", "beta"}, {"newText", "gamma\ndelta"}})["ok"] == true,
                    "Observer edit_file failed");
            Require(svc.ExecuteTool("workspace.move",
                                    {{"from", "src/obs_test.txt"}, {"to", "src/obs_moved.txt"}})["ok"] == true,
                    "Observer move failed");
            Require(svc.ExecuteTool("workspace.delete",
                                    {{"path", "src/obs_moved.txt"}, {"confirm", true}})["ok"] == true,
                    "Observer delete failed");
            svc.RemoveObserver(obsToken);

            bool sawStarted = false, sawDelta = false, sawCompleted = false;
            std::string streamCombined;
            for (const auto &ev : execEvents) {
                if (ev.phase == ExecProgressEvent::Phase::Started && !ev.execId.empty()) sawStarted = true;
                if (ev.phase == ExecProgressEvent::Phase::OutputDelta) {
                    sawDelta = true;
                    streamCombined += ev.chunk;
                }
                if (ev.phase == ExecProgressEvent::Phase::Completed && ev.exitCode == 0) sawCompleted = true;
            }
            Require(sawStarted && sawDelta && sawCompleted &&
                        streamCombined.find("STREAM_OBS_A") != std::string::npos &&
                        streamCombined.find("STREAM_OBS_B") != std::string::npos,
                    "Exec observer must emit Started, OutputDelta and Completed events with stdout");

            Require(fileChanges.size() == 4 &&
                        fileChanges[0].action == "create" && fileChanges[0].path == "src/obs_test.txt" &&
                        fileChanges[0].addedLines == 2 &&
                        fileChanges[1].action == "edit" && fileChanges[1].path == "src/obs_test.txt" &&
                        fileChanges[1].addedLines == 2 && fileChanges[1].removedLines == 1 &&
                        fileChanges[1].diffText.find("-beta") != std::string::npos &&
                        fileChanges[1].diffText.find("+gamma") != std::string::npos &&
                        fileChanges[2].action == "move" && !fileChanges[2].detail.empty() &&
                        fileChanges[2].path == "src/obs_moved.txt" &&
                        fileChanges[3].action == "delete" && fileChanges[3].path == "src/obs_moved.txt",
                    "FileChange observer must record create/edit/move/delete with accurate unifiedDiff and counts");
        }

        // Read-only enforcement
        Require(svc.Configure(true, canonicalRoot, true, err), "Read-only reconfigure failed");
        Require(svc.ExecuteTool("workspace.read_file", {{"path", "src/hello.c"}})["ok"] == true,
                "Read-only blocked read_file");
        for (const auto &blockedCall : std::vector<std::pair<std::string, Json>>{
                 {"workspace.write_file", {{"path", "ro.txt"}, {"content", "x"}}},
                 {"workspace.edit_file", {{"path", "src/hello.c"}, {"oldText", "0"}, {"newText", "1"}}},
                 {"workspace.move", {{"from", "src/hello.c"}, {"to", "src/hello2.c"}}},
                 {"workspace.delete", {{"path", "src/hello.c"}, {"confirm", true}}},
                 {"workspace.exec", {{"command", "echo hi"}}}}) {
            Require(svc.ExecuteTool(blockedCall.first, blockedCall.second)["code"] == "LOCAL_WORKSPACE_READ_ONLY",
                    ("Read-only allowed mutation tool: " + blockedCall.first).c_str());
        }

        // 5. Local Win32 attachment staging & cleanup
        Attachment att;
        att.name = L"sample.txt";
        att.suffix = ".txt";
        const std::string payload = "local-attachment-bytes";
        att.bytes = std::make_shared<serialctl::Bytes>(payload.begin(), payload.end());
        std::vector<std::string> stagedPaths;
        std::wstring stageErr;
        Require(StageLocalAttachments({att}, stagedDir, stagedPaths, stageErr) && stagedPaths.size() == 1,
                "StageLocalAttachments failed");
        Require(std::filesystem::exists(std::filesystem::u8path(stagedPaths[0])),
                "Staged local attachment file missing");
        CleanupLocalAttachmentDirectory(stagedDir);
        Require(!stagedDir.empty() && !std::filesystem::exists(std::filesystem::u8path(stagedPaths[0])),
                "CleanupLocalAttachmentDirectory left staged files behind");
        stagedDir.clear();

        std::cout << "LocalWorkspaceTest: path validation, Win32 enumeration, junction sandbox, AGENTS.md CAS, all 9 built-in tools (including cmd.exe timeout/cancel), read-only lock and local attachments passed\n";
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        if (!stagedDir.empty())
            CleanupLocalAttachmentDirectory(stagedDir);
        std::error_code ec;
        if (!rootDir.empty())
            std::filesystem::remove_all(rootDir, ec);
        if (!outsideDir.empty())
            std::filesystem::remove_all(outsideDir, ec);
        return 1;
    }

    if (!stagedDir.empty())
        CleanupLocalAttachmentDirectory(stagedDir);
    std::error_code ec;
    if (!rootDir.empty())
        std::filesystem::remove_all(rootDir, ec);
    if (!outsideDir.empty())
        std::filesystem::remove_all(outsideDir, ec);
    return 0;
}
