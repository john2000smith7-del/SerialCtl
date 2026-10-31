#pragma once
#include "AiProtocol.h"

namespace serialctl::ai {
// Presentation only. The original result is still returned, unchanged, to the
// App Server. Do not infer execution success from an accepted transport write.
inline std::string PreviewText(const std::string &input, size_t limit = 4096) {
    std::string out;
    out.reserve(std::min(input.size(), limit));
    enum class Escape { None, Start, Csi, String, StringEscape } state = Escape::None;
    for (unsigned char c : input) {
        if (state == Escape::Start) {
            state = c == '[' ? Escape::Csi : (c == ']' || c == 'P' || c == '_' || c == '^')
                ? Escape::String : Escape::None;
            continue;
        }
        if (state == Escape::Csi) { if (c >= 0x40 && c <= 0x7e) state = Escape::None; continue; }
        if (state == Escape::String) {
            if (c == 7) state = Escape::None;
            else if (c == 27) state = Escape::StringEscape;
            continue;
        }
        if (state == Escape::StringEscape) { state = c == '\\' ? Escape::None : Escape::String; continue; }
        if (c == 27) { state = Escape::Start; continue; }
        if (c == '\r') continue;
        if (c < 32 && c != '\n' && c != '\t') continue;
        if (c == 127) continue;
        out.push_back(static_cast<char>(c));
        if (out.size() >= limit) break;
    }
    if (out.size() == limit && input.size() > limit) {
        // Do not leave half a UTF-8 character at the end of a preview, but keep a
        // final character that is complete.
        size_t end = out.size();
        size_t start = end;
        while (start > 0 && end - start < 3 && (static_cast<unsigned char>(out[start - 1]) & 0xc0) == 0x80) --start;
        if (start > 0) {
            const unsigned char lead = static_cast<unsigned char>(out[start - 1]);
            const size_t expected = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
            if (lead >= 0xc0 && end - (start - 1) < expected) end = start - 1;
        }
        out.resize(end);
        out += u8"\n…内容较长，完整输出请查看实时终端或技术详情。";
    }
    return out;
}
inline std::string ToolText(const std::string &tool, const Json &args, const Json &result,
                            const std::string &target) {
    const bool failed = result.is_object() && result.contains("error");
    const std::string where = target.empty() ? std::string() : " · " + target;
    std::string text;
    if (tool == "serialctl_screen") text = u8"读取当前终端屏幕" + where;
    else if (tool == "serialctl_read") text = u8"读取终端日志" + where;
    else if (tool == "serialctl_observe") text = u8"观察终端输出" + where;
    else if (tool == "serialctl_text") text = u8"输入消息" + where;
    else if (tool == "serialctl_key") text = u8"发送按键 " + args.value("key", std::string()) + where;
    else if (tool == "serialctl_command") text = u8"发送命令" + where;
    else if (tool == "serialctl_power_get") text = u8"读取电源状态";
    else if (tool == "serialctl_power_action") text = u8"查询电源操作结果";
    else if (tool == "serialctl_power_output") text = args.value("enabled", false) ? u8"提交电源上电" : u8"提交电源断电";
    else if (tool == "serialctl_boot_enter") text = args.value("preset", std::string()) == "phytium_uefi_f2" ? u8"重启板卡并连续发送 F2" + where : u8"重启板卡并连续发送空格" + where;
    else if (tool == "serialctl_boot_stop") text = u8"停止启动打断" + where;
    else if (tool == "serialctl_boot_arm") text = u8"准备捕捉 U-Boot 启动提示" + where;
    else if (tool == "serialctl_boot_status") text = u8"检查启动固件进入状态" + where;
    else if (tool == "serialctl_recover") text = u8"提交板卡恢复任务" + where;
    else text = u8"查询终端连接";
    if (failed) {
        text += u8"\n操作失败";
        if (result["error"].is_string()) text += "\n" + PreviewText(result["error"].get<std::string>(), 1024);
        return text;
    }
    const bool reading = tool == "serialctl_screen" || tool == "serialctl_read" || tool == "serialctl_observe";
    if (reading) {
        text += u8" · 已读取";
        const auto body = result.value("text", std::string());
        // Cursor-addressed TUI refresh streams cannot be converted to a screen
        // by merely deleting ANSI sequences. Screen tools already return VT text.
        if (tool != "serialctl_screen" && body.find('\x1b') != std::string::npos)
            text += u8"\n收到交互终端数据，当前画面请查看右侧实时终端。";
        else if (!body.empty()) text += "\n```\n" + PreviewText(body) + "\n```";
        else text += u8"\n本次没有新的输出。";
        if (result.value("truncated", false) || result.value("gap", false))
            text += u8"\n本次读取有截断或历史缺口。";
    } else if (tool == "serialctl_key" || tool == "serialctl_text" || tool == "serialctl_command") {
        text += u8"\n已提交，等待终端回显；尚未确认远端执行结果。";
        const auto input = args.value(tool == "serialctl_command" ? "command" : "text", std::string());
        if (!input.empty()) text += "\n```\n" + PreviewText(input, 2048) + "\n```";
    } else if (tool == "serialctl_power_get") {
        text += result.value("connected", false) ? u8"\n电源已连接。" : u8"\n电源未连接。";
    } else if (tool == "serialctl_power_output" || tool == "serialctl_recover") {
        text += u8"\n任务已提交，等待设备回读确认。";
    } else if (tool == "serialctl_boot_arm" || tool == "serialctl_boot_enter" || tool == "serialctl_boot_stop" || tool == "serialctl_boot_status") {
        text += result.value("entered", false) ? u8"\n已观察到目标提示符或菜单。" : u8"\n尚未确认进入目标界面。";
        if(tool=="serialctl_boot_enter") text += u8"\n重启已提交；客户端按 " + std::to_string(result.value("intervalMs",300)) +
            u8" 毫秒间隔持续发送打断按键，不限次数；可点击停止。";
    } else if (tool == "serialctl_power_action") {
        text += u8"\n已取得任务状态；以设备回读结果为准。";
    }
    return text;
}
} // namespace serialctl::ai
