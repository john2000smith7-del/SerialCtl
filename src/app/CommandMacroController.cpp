#include "CommandMacroController.h"
#include "Win32Helpers.h"
#include "FilePaths.h"
#include <io.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>
namespace serialctl {
namespace {
constexpr auto MaximumCommandSteps = CommandMacroController::MaximumSteps;
constexpr auto MaximumCommandIntervalMs = CommandMacroController::MaximumIntervalMs;
std::wstring Trim(std::wstring text) {
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    return text;
}
DWORD ParsePositive(const std::wstring& text, DWORD fallback = 0) {
    wchar_t* end = nullptr; const unsigned long value = wcstoul(text.c_str(), &end, 10);
    return end && *end == L'\0' && value > 0 ? static_cast<DWORD>(value) : fallback;
}
bool ParseMacroHeader(const std::wstring& line, std::uint32_t& intervalMs) {
    constexpr wchar_t Prefix[] = L"[[macro interval=";
    constexpr wchar_t Suffix[] = L"]]";
    const size_t prefixLength = std::size(Prefix) - 1;
    const size_t suffixLength = std::size(Suffix) - 1;
    if (line.size() <= prefixLength + suffixLength || line.rfind(Prefix, 0) != 0 ||
        line.compare(line.size() - suffixLength, suffixLength, Suffix) != 0)
        return false;
    const std::wstring value = line.substr(prefixLength,
        line.size() - prefixLength - suffixLength);
    const DWORD parsed = ParsePositive(value);
    if (parsed == 0 || parsed > MaximumCommandIntervalMs) return false;
    intervalMs = parsed;
    return true;
}

std::wstring EscapeSingleCommandLine(std::wstring line) {
    const size_t firstContent = line.find_first_not_of(L'\\');
    const std::wstring content = firstContent == std::wstring::npos ? std::wstring() : line.substr(firstContent);
    if (content.rfind(L"#", 0) == 0 || content.rfind(L"[[", 0) == 0)
        line.insert(line.begin(), L'\\');
    return line;
}

std::wstring EscapeMacroCommandLine(std::wstring line) {
    const size_t firstContent = line.find_first_not_of(L'\\');
    if (firstContent != std::wstring::npos && line.substr(firstContent) == L"[[/macro]]")
        line.insert(line.begin(), L'\\');
    return line;
}

bool UnescapeSingleCommandLine(std::wstring& line) {
    const size_t firstContent = line.find_first_not_of(L'\\');
    if (firstContent == 0 || firstContent == std::wstring::npos) return false;
    const std::wstring content = line.substr(firstContent);
    if (content.rfind(L"#", 0) != 0 && content.rfind(L"[[", 0) != 0) return false;
    line.erase(0, 1);
    return true;
}

void UnescapeMacroCommandLine(std::wstring& line) {
    const size_t firstContent = line.find_first_not_of(L'\\');
    if (firstContent != 0 && firstContent != std::wstring::npos &&
        line.substr(firstContent) == L"[[/macro]]")
        line.erase(0, 1);
}
} // namespace
bool CommandMacroController::LoadFile(const std::wstring& path, std::wstring& error) {
    if (Running()) { error = L"宏执行期间不能替换命令。"; return false; }
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || !file) { error = L"无法打开命令文件。"; return false; }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0 || size > 4 * 1024 * 1024) { fclose(file); error = L"命令文件大小无效或超过 4 MB。"; return false; }
    std::string bytes(static_cast<size_t>(size), '\0');
    if (!bytes.empty() && fread(bytes.data(), 1, bytes.size(), file) != bytes.size()) { fclose(file); error = L"命令文件读取失败。"; return false; }
    fclose(file);
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) bytes.erase(0, 3);
    if (!bytes.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0)) { error = L"命令文件不是有效 UTF-8。"; return false; }
    const std::wstring content = MultiByteToWide(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size(), CP_UTF8);
    std::vector<CommandItem> parsed;
    std::wstring pendingName;
    CommandItem pendingMacro;
    bool readingMacro = false;
    size_t start = 0;
    while (start <= content.size()) {
        const size_t end = content.find(L'\n', start);
        std::wstring line = Trim(content.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (!line.empty()) {
            if (readingMacro) {
                if (line == L"[[/macro]]") {
                    if (pendingMacro.commands.empty()) {
                        error = L"宏中没有有效指令。";
                        return false;
                    }
                    if (pendingMacro.name.empty()) pendingMacro.name = pendingMacro.commands.front();
                    parsed.push_back(std::move(pendingMacro));
                    pendingMacro = {};
                    readingMacro = false;
                } else {
                    UnescapeMacroCommandLine(line);
                    pendingMacro.commands.push_back(std::move(line));
                    if (pendingMacro.commands.size() > MaximumCommandSteps) {
                        error = L"每个宏最多包含 " + std::to_wstring(MaximumCommandSteps) + L" 条指令。";
                        return false;
                    }
                }
            } else if (UnescapeSingleCommandLine(line)) {
                parsed.push_back({pendingName.empty() ? line : pendingName, line});
                pendingName.clear();
            } else if (line.front() == L'#') {
                pendingName = Trim(line.substr(1));
            } else {
                std::uint32_t intervalMs = 0;
                if (ParseMacroHeader(line, intervalMs)) {
                    pendingMacro = {};
                    pendingMacro.name = pendingName;
                    pendingMacro.intervalMs = intervalMs;
                    pendingName.clear();
                    readingMacro = true;
                } else if (line.rfind(L"[[macro", 0) == 0) {
                    error = L"宏开始标记无效，指令间隔必须是 1 到 60000 毫秒。";
                    return false;
                } else if (line == L"[[/macro]]") {
                    error = L"命令文件中存在没有开始标记的宏结束行。";
                    return false;
                } else {
                    parsed.push_back({pendingName.empty() ? line : pendingName, line});
                    pendingName.clear();
                }
            }
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    if (readingMacro) {
        error = L"命令文件中的宏缺少 [[/macro]] 结束标记。";
        return false;
    }
    items_ = std::move(parsed);
    return true;
}


bool CommandMacroController::SaveFile(const std::wstring& path, std::wstring& error) const {
    FILE* file = nullptr;
    const auto destination = ExtendedFilePath(path, true);
    const auto slash = destination.find_last_of(L"\\/");
    std::wstring temporary;
    if (destination.empty() || slash == std::wstring::npos ||
        !CreateUniqueTemporaryFile(destination.substr(0, slash), temporary)) {
        error = L"无法保存命令文件。"; return false;
    }
    if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || !file) {
        DeleteFileW(temporary.c_str()); error = L"无法保存命令文件。"; return false;
    }
    bool wrote = true;
    const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
    wrote = fwrite(bom, 1, sizeof(bom), file) == sizeof(bom);
    for (const CommandItem& item : items_) {
        std::wstring block = L"#" + item.name + L"\r\n";
        if (item.commands.size() > 1) {
            block += L"[[macro interval=" + std::to_wstring(item.intervalMs) + L"]]\r\n";
            for (const std::wstring& command : item.commands)
                block += EscapeMacroCommandLine(command) + L"\r\n";
            block += L"[[/macro]]\r\n";
        } else if (!item.commands.empty()) {
            block += EscapeSingleCommandLine(item.commands.front()) + L"\r\n";
        }
        const std::string utf8 = WideToMultiByte(block, CP_UTF8);
        if (fwrite(utf8.data(), 1, utf8.size(), file) != utf8.size()) wrote = false;
    }
    if (fflush(file) != 0 || _commit(_fileno(file)) != 0) wrote = false;
    if (fclose(file) != 0) wrote = false;
    if (!wrote || !MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str()); error = L"保存失败，原命令文件已保留。"; return false;
    }
    return true;
}



bool CommandMacroController::ImportFile(const std::wstring& source, const std::wstring& protectedPath,
                                         std::wstring& error) {
    if (Running()) { error = L"宏执行期间不能替换命令。"; return false; }
    CommandMacroController candidate;
    if (!candidate.LoadFile(source, error)) return false;
    if (protected_) {
        const auto backup = protectedPath + L".unreadable-" + std::to_wstring(GetTickCount64()) + L".bak";
        if (!CopyFileW(ExtendedFilePath(protectedPath).c_str(), ExtendedFilePath(backup).c_str(), TRUE)) {
            error = L"无法备份原命令文件，尚未允许覆盖。"; return false;
        }
    }
    items_ = std::move(candidate.items_); dirty_ = true; protected_ = false;
    return true;
}

bool CommandMacroController::Replace(std::vector<CommandItem> items, bool dirty) {
    if (Running()) return false; items_ = std::move(items); dirty_ = dirty; return true;
}
bool CommandMacroController::Update(size_t index, CommandItem item) {
    if (Running() || index >= items_.size()) return false;
    items_[index] = std::move(item); dirty_ = true; return true;
}
bool CommandMacroController::Append(CommandItem item) {
    if (Running()) return false; items_.push_back(std::move(item)); dirty_ = true; return true;
}
bool CommandMacroController::Erase(size_t index) {
    if (Running() || index >= items_.size()) return false;
    items_.erase(items_.begin() + index); dirty_ = true; return true;
}
bool CommandMacroController::Move(size_t from, size_t to) {
    if (Running() || from >= items_.size() || to >= items_.size() || from == to) return false;
    auto item = std::move(items_[from]); items_.erase(items_.begin() + from);
    items_.insert(items_.begin() + to, std::move(item)); dirty_ = true; return true;
}
bool CommandMacroController::Duplicate(size_t index, std::uint64_t session, std::uint64_t now, std::uint32_t window) const {
    return lastIndex_ == static_cast<int>(index) && lastSession_ == session && now - lastActivation_ < window;
}
void CommandMacroController::Activated(size_t index, std::uint64_t session, std::uint64_t now) {
    lastIndex_ = static_cast<int>(index); lastSession_ = session; lastActivation_ = now;
}
bool CommandMacroController::Begin(size_t index, std::uint64_t session, std::uint32_t codePage, std::wstring lineEnding) {
    if (Running() || index >= items_.size() || items_[index].commands.size() < 2) return false;
    plan_ = items_[index]; runningIndex_ = static_cast<int>(index); session_ = session;
    codePage_ = codePage; lineEnding_ = std::move(lineEnding); accepted_ = 0; return true;
}
bool CommandMacroController::Next(std::uint64_t currentSession, bool connected, std::wstring& text) const {
    if (!Running() || currentSession != session_ || !connected || accepted_ >= Total()) return false;
    text = plan_.commands[accepted_] + lineEnding_; return true;
}
CommandMacroController::Completion CommandMacroController::Stop(End end, std::wstring detail) {
    Completion result;
    if (!Running()) return result;
    result.active = true; result.session = session_; result.accepted = accepted_; result.total = Total();
    result.error = end == End::Interrupted;
    const auto count = std::to_wstring(accepted_) + L"/" + std::to_wstring(Total());
    if (end == End::Submitted) result.message = L"宏“" + plan_.name + L"”已提交 " + count + L" 条；执行结果见输出。";
    else if (end == End::UserStopped) result.message = L"宏“" + plan_.name + L"”已停止后续提交（" + count + L"）；已提交内容可能继续执行，结果见输出。";
    else if (end == End::Interrupted) result.message = L"宏“" + plan_.name + L"”后续提交已中断（" + count + L"）：" + detail + L"；已提交内容执行状态未知，请检查连接和输出，不自动重发。";
    runningIndex_ = -1; session_ = 0; accepted_ = 0; plan_ = {}; lineEnding_.clear(); return result;
}
} // namespace serialctl
