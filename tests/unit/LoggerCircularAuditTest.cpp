#include "OperationAuditLogger.h"
#include "SessionLogger.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <windows.h>

using namespace serialctl;

namespace {

int g_failures = 0;

void Expect(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAILED: " << msg << '\n';
        ++g_failures;
    }
}

std::wstring CreateTempDir() {
    wchar_t tmp[MAX_PATH]{};
    const DWORD n = GetTempPathW(MAX_PATH, tmp);
    if (n == 0 || n >= MAX_PATH) {
        std::cerr << "GetTempPathW failed\n";
        std::exit(1);
    }
    const std::wstring dir = std::wstring(tmp, n) + L"serialctl-logger-unit-" +
                             std::to_wstring(GetCurrentProcessId());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::filesystem::create_directories(dir, ec);
    return dir;
}

} // namespace

int main() {
    const std::wstring tempDir = CreateTempDir();
    const std::wstring auditDir = tempDir + L"\\audit";

    // 1. DefaultLogDirectory resolves to <exe_dir>\logs (under exe folder)
    wchar_t exePath[MAX_PATH]{};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    const std::wstring expectedDefaultDir =
        std::filesystem::path(exePath).parent_path().wstring() + L"\\logs";
    Expect(SessionLogger::DefaultLogDirectory() == expectedDefaultDir,
           "DefaultLogDirectory resolves next to executable");

    // 2. SessionLogger circular part eviction & EmergencyFlushAll
    {
        SessionLogger logger;
        logger.ConfigureRotation(3, 512); // max 3 parts, 512 bytes/part for fast unit verification
        std::wstring err;
        Expect(logger.Start(L"COM7_unit", err), "SessionLogger starts");
        const std::wstring basePath = logger.Path();
        Expect(!basePath.empty(), "SessionLogger reports base path");

        const std::wstring chunk(200, L'Z');
        for (int i = 0; i < 12; ++i) {
            logger.WriteText(chunk + L"\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        SessionLogger::EmergencyFlushAll();
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        logger.Stop();

        const auto parentDir = std::filesystem::path(basePath).parent_path();
        const std::wstring stem = std::filesystem::path(basePath).stem().wstring();
        int matchingParts = 0;
        for (const auto& entry : std::filesystem::directory_iterator(parentDir)) {
            const std::wstring name = entry.path().filename().wstring();
            if (name.find(stem) != std::wstring::npos) {
                ++matchingParts;
            }
        }
        Expect(matchingParts >= 1 && matchingParts <= 3,
               "circular rotation enforces <= maxPartsPerSession parts on disk");

        // Clean up session log files created under exe\logs during test
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(parentDir, ec)) {
            const std::wstring name = entry.path().filename().wstring();
            if (name.find(stem) != std::wstring::npos) {
                std::filesystem::remove(entry.path(), ec);
            }
        }
    }

    // 3. OperationAuditLogger circular JSONL recording, emergency flush, export, clear
    auto& audit = OperationAuditLogger::Instance();
    audit.Configure(auditDir, 3, 512);
    audit.SetAutoDeleteOnExit(false);
    for (int i = 0; i < 10; ++i) {
        audit.Record("[MANUAL]", "gui", "power.on",
                     "{\"step\":" + std::to_string(i) + "}", "ok");
        audit.Record("[AI]", "ai:agent", "serial.write",
                     "{\"cmd\":\"ls\"}", "ok");
        audit.Record("[MCP]", "mcp:1", "power.off",
                     "{}", "ok");
    }
    OperationAuditLogger::EmergencyFlushAll();
    Expect(audit.RecordedCount() >= 30, "all 30 audit entries counted");

    int auditFileCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(auditDir)) {
        if (entry.path().extension() == L".jsonl")
            ++auditFileCount;
    }
    Expect(auditFileCount >= 1 && auditFileCount <= 3,
           "audit logger maintains bounded circular JSONL files");

    const std::wstring exported = tempDir + L"\\exported-audit.jsonl";
    std::wstring err;
    Expect(audit.ExportTo(exported, err), "ExportTo merges circular audit logs");
    {
        std::ifstream in(exported);
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        Expect(content.find("[MANUAL]") != std::string::npos, "export contains [MANUAL]");
        Expect(content.find("[AI]") != std::string::npos, "export contains [AI]");
        Expect(content.find("[MCP]") != std::string::npos, "export contains [MCP]");
    }

    Expect(audit.ClearHistory(err), "ClearHistory removes audit logs");
    Expect(audit.RecordedCount() == 0, "RecordedCount resets on clear");
    audit.Shutdown();

    std::error_code ec;
    std::filesystem::remove_all(tempDir, ec);
    return g_failures == 0 ? 0 : 1;
}
