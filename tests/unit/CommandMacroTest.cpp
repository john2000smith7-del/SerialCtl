#include "CommandMacroController.h"
#include "CommandDocument.h"
#include "FilePaths.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace serialctl;
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void Write(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream file(path, std::ios::binary); file.write(bytes.data(), bytes.size());
    Check(file.good(), "write controlled command fixture");
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}
int main() {
    auto directory = std::filesystem::temp_directory_path() /
        (L"SerialCtl-macro-" + std::to_wstring(GetCurrentProcessId()));
    struct Cleanup { std::filesystem::path path; ~Cleanup() { std::error_code error; std::filesystem::remove_all(path, error); } } cleanup{directory};
    try {
        std::filesystem::create_directories(directory);
        const auto path = directory / L"中文 commands.txt";
        // Real selection boundaries: partial text, reversed drags, CR/LF and Unicode offsets.
        const std::wstring document=L"# 标题\r\nuname -a\r\ncat /proc/cmdline\r\ndmesg | tail -n 20\r\n\r\n# 注释";
        const auto rows=CommandLines(document);
        const auto selected=CommandSelection(document,rows[1].start+3,rows[3].start+5);
        Check(selected==std::vector<std::wstring>{L"uname -a",L"cat /proc/cmdline",L"dmesg | tail -n 20"},"partial multiline selection expands to complete physical commands");
        Check(CommandSelection(document,rows[3].start+5,rows[1].start+3)==selected,"reverse selection retains source order");
        Check(CommandSelection(document,rows[1].start,rows[2].start)==std::vector<std::wstring>{L"uname -a"},"selection ending at next line column one excludes that untouched line");
        Check(CommandSelection(document,rows[2].start+4,rows[2].start+4)==std::vector<std::wstring>{L"cat /proc/cmdline"},"empty selection uses caret line");
        Check(CommandSelection(L"# note\n\n[[macro interval=500]]\rhelp\r[[/macro]]",0,1000)==std::vector<std::wstring>{L"help"},"comments, blanks and legacy macro delimiters never sent");
        std::wstring documentError;
        const auto plain=directory/L"笔记.txt";
        Check(WriteCommandDocument(plain.wstring(),document,documentError),"atomic notebook UTF8 save");
        std::wstring restored;
        Check(ReadCommandDocument(plain.wstring(),restored,documentError)&&restored==document,"notebook round trip preserves whitespace and comments");
        CommandMacroController controller;
        CommandItem plan(L"顺序提交", L"first"); plan.commands.push_back(L"second"); plan.intervalMs = 1;
        Check(controller.Replace({plan, {L"字面量", L"[[/macro]]"}}), "replace command list");
        Check(controller.Move(1, 0) && controller.Items()[0].name == L"字面量" && controller.Dirty(), "committed reorder belongs to controller");
        std::wstring error;
        Check(controller.SaveFile(path.wstring(), error), "atomic UTF-8 save");
        const auto saved = Read(path);
        Check(saved.rfind("\xef\xbb\xbf", 0) == 0 && saved.find("\\[[/macro]]\r\n") != std::string::npos,
              "BOM, CRLF and escaped literal markers retained");
        CommandMacroController loaded;
        Check(loaded.LoadFile(path.wstring(), error) && loaded.Items().size() == 2 &&
              loaded.Items()[0].commands.front() == L"[[/macro]]" && loaded.Items()[1].commands == plan.commands,
              "legacy command/macro format round trips");
        const auto invalid = directory / L"invalid.txt";
        for (const std::string bytes : {"[[macro interval=0]]\nfirst\n[[/macro]]\n", "[[macro interval=1]]\nfirst\n", "\xff\xfe", "[[/macro]]\n"}) {
            Write(invalid, bytes);
            Check(!loaded.LoadFile(invalid.wstring(), error) && loaded.Items().size() == 2 &&
                  loaded.Items()[0].commands.front() == L"[[/macro]]", "failed parsing retains active commands");
        }
        Check(SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_READONLY) != FALSE, "protect destination fixture");
        const bool refused = controller.SaveFile(path.wstring(), error);
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        Check(!refused && Read(path) == saved, "failed atomic replacement retains old file");
        for (const auto& file : std::filesystem::directory_iterator(directory))
            Check(file.path().extension() != L".tmp", "failed save removes only its unique temporary file");
        loaded.MarkSaved(); loaded.ProtectFile(true);
        Check(!loaded.ImportFile(path.wstring(), (directory / L"absent.txt").wstring(), error) &&
              loaded.FileProtected() && !loaded.Dirty(), "backup failure retains protected state and command list");
        Check(loaded.ImportFile(path.wstring(), path.wstring(), error) && loaded.Dirty() && !loaded.FileProtected(),
              "validated import creates backup before allowing replacement");
        Check(loaded.Begin(1, 42, 936, L"\r"), "begin binds immutable plan to one session and encoding");
        Check(!loaded.Move(0, 1) && !loaded.Erase(0) && !loaded.Replace({}), "running plan cannot be replaced or reordered");
        std::wstring next;
        Check(!loaded.Next(43, true, next) && !loaded.Next(42, false, next), "session change/disconnect cannot submit another step");
        Check(loaded.Next(42, true, next) && next == L"first\r" && loaded.Accepted() == 0, "reading next step never assumes acceptance");
        loaded.Accept();
        auto stopped = loaded.Stop(CommandMacroController::End::Interrupted, L"fixture write failure");
        Check(stopped.active && stopped.error && stopped.session == 42 && stopped.accepted == 1 && stopped.total == 2 &&
              stopped.message.find(L"执行状态未知") != std::wstring::npos && !loaded.Running(),
              "interruption records accepted steps without automatic retry");
        Check(!loaded.Stop(CommandMacroController::End::UserStopped).active, "repeated stop is inert");
        Check(loaded.Begin(1, 42, 65001, L"\r\n"), "restart is explicit");
        loaded.Accept(); loaded.Accept();
        Check(loaded.Complete() && loaded.Stop(CommandMacroController::End::Submitted).accepted == 2, "all accepted steps complete sequence");
        loaded.Activated(1, 42, 1000);
        Check(loaded.Duplicate(1, 42, 1100, 350) && !loaded.Duplicate(1, 43, 1100, 350) &&
              !loaded.Duplicate(1, 42, 1350, 350), "activation guard preserves session and exact repeat boundary");
        std::cout << "Command ownership, encoding/session plan, partial submission and safe legacy configuration passed\n";
    } catch (const std::exception& error) { std::cerr << "FAILED: " << error.what() << '\n'; return 1; }
    return 0;
}
