#include "ToolPresentation.h"
#include <iostream>
#include <stdexcept>
using namespace serialctl::ai;
void Check(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
int main() {
    try {
        Json screen{{"sessionId", "session-1"}, {"text", u8"正常中文\n第二行"},
                    {"alternateScreen", true}, {"cursor", 8455}, {"truncated", false}};
        const auto original = screen;
        auto text = ToolText("serialctl_screen", Json::object(), screen, "SSH");
        Check(text.find(u8"正常中文\n第二行") != std::string::npos, "Screen lost real newlines or Chinese");
        Check(text.find("sessionId") == std::string::npos && text.find("\\n") == std::string::npos,
              "Protocol envelope escaped into readable screen");
        Check(screen == original, "Presentation changed evidence sent to the model");
        text = ToolText("serialctl_key", {{"key", "Enter"}}, {{"accepted", 5}, {"execution", "unknown"}}, "SSH");
        Check(text.find("Enter") != std::string::npos && text.find(u8"等待") != std::string::npos &&
              text.find(u8"执行成功") == std::string::npos && text.find("accepted") == std::string::npos,
              "Transport acceptance was presented as device execution success");
        const std::string ansi = "\x1b[2;10Hhello\x1b[0m\r\n\x1b]0;private-title\aoutput";
        text = ToolText("serialctl_read", Json::object(), {{"text", ansi}}, "SSH");
        Check(text.find('\x1b') == std::string::npos && text.find("2;10H") == std::string::npos &&
              text.find(u8"实时终端") != std::string::npos, "Raw TUI refresh stream exposed as log text");
        Check(PreviewText(ansi) == "hello\noutput", "Preview failed to filter CSI/OSC and preserve newline");
        text = ToolText("serialctl_read", Json::object(), {{"text", "ok"}, {"truncated", true}}, "COM3");
        Check(text.find(u8"截断") != std::string::npos, "Truncation warning was hidden");
        const auto huge = PreviewText(std::string(100000, 'x'));
        Check(huge.size() < 4500 && huge.find(u8"完整输出") != std::string::npos, "Preview was not bounded");
        std::string chinese;
        for (int i = 0; i < 3000; ++i) chinese += u8"中";
        Check(!Json(PreviewText(chinese)).dump(-1, ' ', true).empty(), "Bounded UTF-8 preview broke encoding");
        text = ToolText("serialctl_screen", Json::object(), {{"error", "Connection closed"}}, "SSH");
        Check(text.find(u8"失败") != std::string::npos && text.find(u8"已读取") == std::string::npos,
              "Failed read appeared successful");
        std::cout << "Readable tool summaries, actual newlines, ANSI isolation, bounded UTF-8 and unchanged evidence PASS\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
