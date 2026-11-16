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
    std::string title;
    if (tool == "serialctl_screen") title = u8"读取当前终端屏幕" + where;
    else if (tool == "serialctl_read") title = u8"读取终端日志" + where;
    else if (tool == "serialctl_observe") title = u8"观察终端输出" + where;
    else if (tool == "serialctl_text") title = u8"输入消息" + where;
    else if (tool == "serialctl_key") title = u8"发送按键 `" + args.value("key", std::string()) + "`" + where;
    else if (tool == "serialctl_command") title = u8"发送命令" + where;
    else if (tool == "serialctl_power_get") title = u8"读取电源状态";
    else if (tool == "serialctl_power_action") title = u8"查询电源操作结果";
    else if (tool == "serialctl_power_output") title = args.value("enabled", false) ? u8"提交电源上电" : u8"提交电源断电";
    else if (tool == "serialctl_boot_enter") title = args.value("preset", std::string()) == "phytium_uefi_f2" ? u8"重启板卡并连续发送 F2" + where : u8"重启板卡并连续发送空格" + where;
    else if (tool == "serialctl_boot_stop") title = u8"停止启动打断" + where;
    else if (tool == "serialctl_boot_arm") title = u8"准备捕捉 U-Boot 启动提示" + where;
    else if (tool == "serialctl_boot_status") title = u8"检查启动固件进入状态" + where;
    else if (tool == "serialctl_recover") title = u8"提交板卡恢复任务" + where;
    else if (tool == "mcp_workspace_info") title = u8"查询本地工作空间状态";
    else if (tool == "mcp_workspace_list") title = u8"列出本地工作空间目录 · `" + args.value("path", std::string(".")) + "`";
    else if (tool == "mcp_workspace_search_text") title = u8"检索本地工作空间文本 · `" + args.value("query", std::string()) + "`";
    else if (tool == "mcp_workspace_read_file") title = u8"读取本地工作空间文件 · `" + args.value("path", std::string()) + "`";
    else if (tool == "mcp_workspace_write_file") title = u8"写入本地工作空间文件 · `" + args.value("path", std::string()) + "`";
    else if (tool == "mcp_workspace_edit_file") title = u8"修改本地工作空间文件 · `" + args.value("path", std::string()) + "`";
    else if (tool == "mcp_workspace_move") title = u8"移动本地工作空间路径 · `" + args.value("from", std::string()) + "`";
    else if (tool == "mcp_workspace_delete") title = u8"删除本地工作空间条目 · `" + args.value("path", std::string()) + "`";
    else if (tool == "mcp_workspace_exec") title = u8"在本地工作空间执行命令";
    else if (tool.rfind("mcp_", 0) == 0) title = u8"调用 MCP 工具 `" + tool.substr(4) + "`" + where;
    else title = u8"查询终端连接";
    std::string text = "**" + title + "**";
    if (failed) {
        text += u8" · 失败";
        if (result["error"].is_string())
            text += "\n\n```text\n" + PreviewText(result["error"].get<std::string>(), 1024) + "\n```";
        return text;
    }
    const bool reading = tool == "serialctl_screen" || tool == "serialctl_read" || tool == "serialctl_observe";
    if (reading) {
        text += u8" · 已读取";
        const auto body = result.value("text", std::string());
        // Cursor-addressed TUI refresh streams cannot be converted to a screen
        // by merely deleting ANSI sequences. Screen tools already return VT text.
        if (tool != "serialctl_screen" && body.find('\x1b') != std::string::npos)
            text += u8"\n\n收到交互终端数据，当前画面请查看右侧实时终端。";
        else if (!body.empty()) text += "\n\n```text\n" + PreviewText(body) + "\n```";
        else text += u8"\n\n本次没有新的输出。";
        if (result.value("truncated", false) || result.value("gap", false))
            text += u8"\n\n本次读取有截断或历史缺口。";
    } else if (tool == "serialctl_key" || tool == "serialctl_text" || tool == "serialctl_command") {
        text += u8" · 已提交\n\n等待终端回显，尚未确认远端执行结果。";
        const auto input = args.value(tool == "serialctl_command" ? "command" : "text", std::string());
        if (!input.empty()) text += "\n\n```text\n" + PreviewText(input, 2048) + "\n```";
    } else if (tool == "serialctl_power_get") {
        text += result.value("connected", false) ? u8" · 已连接" : u8" · 未连接";
    } else if (tool == "serialctl_power_output" || tool == "serialctl_recover") {
        text += u8" · 已提交\n\n等待设备回读确认。";
    } else if (tool == "serialctl_boot_arm" || tool == "serialctl_boot_enter" || tool == "serialctl_boot_stop" || tool == "serialctl_boot_status") {
        text += result.value("entered", false) ? u8" · 已进入目标界面" : u8" · 尚未确认进入";
        if(tool=="serialctl_boot_enter") text += u8"\n\n重启已提交；客户端按 " + std::to_string(result.value("intervalMs",300)) +
            u8" 毫秒间隔持续发送打断按键，不限次数；可点击停止。";
    } else if (tool == "serialctl_power_action") {
        text += u8" · 已获取状态\n\n以设备回读结果为准。";
    } else if (tool == "mcp_workspace_read_file") {
        text += u8" · 已读取";
        const auto content = result.value("content", std::string());
        if (!content.empty())
            text += "\n\n```text\n" + PreviewText(content, 2048) + "\n```";
    } else if (tool == "mcp_workspace_search_text") {
        const auto &matches = result.contains("matches") && result["matches"].is_array() ? result["matches"] : Json::array();
        text += u8" · 命中 " + std::to_string(matches.size()) + u8" 处";
        if (!matches.empty()) {
            std::string body;
            const size_t shown = std::min<size_t>(matches.size(), 10);
            for (size_t i = 0; i < shown; ++i) {
                const auto &m = matches[i];
                if (!m.is_object()) continue;
                body += m.value("path", m.value("file", std::string())) + ":" +
                        std::to_string(m.value("line", 0)) + ": " +
                        m.value("text", m.value("lineText", std::string())) + "\n";
            }
            if (matches.size() > shown)
                body += u8"… 其余 " + std::to_string(matches.size() - shown) + u8" 处见技术详情";
            if (!body.empty()) text += "\n\n```text\n" + PreviewText(body, 2048) + "\n```";
        }
    } else if (tool == "mcp_workspace_list") {
        const auto &entries = result.contains("entries") && result["entries"].is_array() ? result["entries"] : Json::array();
        text += u8" · 共 " + std::to_string(entries.size()) + u8" 项";
        if (!entries.empty()) {
            std::string body;
            const size_t shown = std::min<size_t>(entries.size(), 16);
            for (size_t i = 0; i < shown; ++i) {
                const auto &entry = entries[i];
                if (entry.is_string()) {
                    body += entry.get<std::string>() + "\n";
                } else if (entry.is_object()) {
                    auto p = entry.value("path", entry.value("name", std::string()));
                    if (entry.value("isDirectory", false) || entry.value("type", std::string()) == "directory")
                        p += "/";
                    if (!p.empty()) body += p + "\n";
                }
            }
            if (entries.size() > shown)
                body += u8"… 其余 " + std::to_string(entries.size() - shown) + u8" 项见技术详情";
            if (!body.empty()) text += "\n\n```text\n" + PreviewText(body, 2048) + "\n```";
        }
    } else if (tool == "mcp_workspace_exec") {
        text += u8" · 退出码 " + std::to_string(result.value("exitCode", -1));
        const auto cmd = args.value("command", std::string());
        if (!cmd.empty())
            text += "\n\n```cmd\n" + PreviewText(cmd, 1024) + "\n```";
        const auto out = result.value("stdout", std::string());
        if (!out.empty())
            text += u8"\n\n**输出**\n\n```text\n" + PreviewText(out, 2048) + "\n```";
        const auto errOut = result.value("stderr", std::string());
        if (!errOut.empty())
            text += u8"\n\n**错误输出**\n\n```text\n" + PreviewText(errOut, 1024) + "\n```";
    } else if (tool.rfind("mcp_workspace_", 0) == 0) {
        text += result.value("ok", false) ? u8" · 已完成" : u8" · 已返回";
    }
    return text;
}
} // namespace serialctl::ai
