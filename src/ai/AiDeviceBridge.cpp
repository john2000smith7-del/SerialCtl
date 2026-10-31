#include "AiDeviceBridge.h"
#include "AiProtocol.h"
#include "Win32Helpers.h"
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <objbase.h>
namespace serialctl {
namespace {
std::wstring Trim(std::wstring text) {
    while (!text.empty() && iswspace(text.front())) text.erase(text.begin());
    while (!text.empty() && iswspace(text.back())) text.pop_back();
    return text;
}
}
Json AiDeviceBridge::Send(Target &session, const Bytes &bytes) {
    if (bytes.empty() || bytes.size() > 65536 || !session.connection || !session.connection->IsConnected())
        return {{"error", "Terminal is disconnected or input exceeds its bound"}};
    const auto id = "session-" + std::to_string(session.id);
    if (session.mode == 1 || session.mode == 4)
        return sessions_.Input(id, bytes, "ai:desktop");
    std::wstring error;
    if (!session.connection->Send(bytes, error)) return {{"error", WideToMultiByte(error, CP_UTF8)}};
    return {{"accepted", bytes.size()}, {"state", "queued"}, {"execution", "unknown"},
        {"scope", "existing-selected-terminal-connection"}, {"automaticRetry", false}};
}
Json AiDeviceBridge::Request(const std::string& method, const Json& params, std::vector<Target>& targets,
                             const Context& context, ULONGLONG now, bool& armBootPulse) {
    armBootPulse = false;
    auto error = [](const std::string& text) -> Json { return {{"error", text}}; };
    if (method == "sessions") {
        Json result = Json::array();
        for (const auto& item : targets) {
            const auto* session = &item;
            if (*ai::TerminalKind(session->mode) && session->connection && session->connection->IsConnected())
                result.push_back(
                    {{"id", "session-" + std::to_string(session->id)},
                     {"instance", context.instance},
                     {"name", std::string("[") + ai::TerminalKind(session->mode) + "] " +
                                  WideToMultiByte(session->name, CP_UTF8)},
                     {"kind", ai::TerminalKind(session->mode)},
                     {"username", session->mode == 0 ? WideToMultiByte(session->username,CP_UTF8) : ""},
                     {"codePage", session->connection->OutputCodePage() ? session->connection->OutputCodePage()
                                                                        : session->codePage},
                     {"inputCodePage",
                      session->connection->InputCodePage() ? session->connection->InputCodePage() : session->codePage},
                     {"remoteIp", session->mode == 0 || session->mode == 2 || session->mode == 3
                                      ? WideToMultiByte(session->host, CP_UTF8)
                                      : ""},
                     {"remotePort", session->mode == 0 || session->mode == 2 || session->mode == 3 ? session->port : 0},
                     {"remoteCom", WideToMultiByte(session->remoteCom, CP_UTF8)},
                     {"com", WideToMultiByte(session->remoteCom, CP_UTF8)},
                     {"transport", session->udpRaw ? "udpRaw" : "session"},
                     {"peerVerified", !session->udpRaw},
                     {"dataReceived", session->udpReceived},
                     {"commandSupported", true}, {"bootPreset", session->state.bootPreset}, {"bootIntervalMs", session->state.bootIntervalMs}});
        }
        return result;
    }
    if (method == "power.get") {
        const auto state = power_.State();
        Json result = {{"id", "power-1"}, {"selectedChannels", context.selectedChannels}};
        result["recoveryBinding"] = context.recoveryBinding;
        for (const char* key : {"connected", "model", "identity", "simulation", "mode", "channels", "task", "error"})
            if (state.contains(key)) result[key] = state[key];
        return result;
    }
    if (method == "power.output") {
        if (!params.contains("enabled") || !params["enabled"].is_boolean() || !params.contains("channels") ||
            params["channels"] != context.selectedChannels)
            return error("Power channel selection changed; query power state again. Nothing submitted");
        const auto state = power_.State();
        if (!state.value("connected", false)) return error("Power supply is not connected");
        if (params.value("identity", std::string()) != state.value("identity", std::string()) ||
            params.value("mode", std::string()) != state.value("mode", std::string()))
            return error("Power identity/mode changed; query state again. Nothing submitted");
        return power_.Submit({{"type", "output"}, {"source", "ai:desktop"},
            {"channels", params["channels"]}, {"enabled", params["enabled"]},
            {"requestId", params.at("requestId")}});
    }
    if (method == "power.action") return power_.Action(params.at("id").get<std::string>());
    const auto id = params.value("sessionId", std::string());
    Target* session = nullptr;
    for (auto& candidate : targets) if (id == "session-" + std::to_string(candidate.id)) { session = &candidate; break; }
    if (method == "release") {
        if (session) {
            session->state.boot->Cancel();
            session->state.bootRestartUsed = false;
            const auto task = power_.State().value("task", Json::object());
            if (!session->state.recoveryAction.empty() && task.value("running", false) &&
                task.value("actionId", std::string()) == session->state.recoveryAction)
                power_.Submit({{"type", "stop"}, {"source", "ai-turn-ended"}});
            session->state.recoveryAction.clear();
        }
        return {{"released", true}};
    }
    if (!session || !*ai::TerminalKind(session->mode) || !session->connection || !session->connection->IsConnected())
        return error("Associated terminal session is not open; no connection was opened automatically");
    if (method == "boot_enter") {
        if (session->state.bootRestartUsed) return error("A reboot was already submitted in this task; query boot_status instead of resubmitting");
        if ((session->mode != 1 && session->mode != 3) || context.macroRunning)
            return error("Boot entry requires the explicitly selected idle board serial connection");
        const auto preset = params.value("preset", std::string());
        const auto command = params.value("command", std::string("reboot"));
        if (command != "reboot" && command != "reset") return error("Boot entry accepts only reboot or a verified board reset command");
        if (!session->state.boot->PreparePreset(preset, params.value("timeoutSeconds", 0u), now, params.value("intervalMs", session->state.bootIntervalMs)))
            return error("Unknown boot preset, invalid timeout, or another capture is already active; do not resubmit reboot");
        // Pin command and interrupts to the same existing connection, in order.
        // Reboot is submitted exactly once, without a model round trip before
        // the first interrupt. This does not open a connection or switch power.
        std::string outgoing = command;
        const char* endings[] = {"\r", "\n", "\r\n"};
        outgoing += endings[std::clamp(session->lineEndingIndex, 0, 2)];
        auto result = Send(*session, Bytes(outgoing.begin(), outgoing.end()));
        if (result.contains("error")) {
            const auto& failure=result["error"];
            std::string reason="Restart submission failed";
            if(failure.is_string()) reason=failure.get<std::string>();
            else if(failure.is_object() && failure.contains("message") && failure["message"].is_string())
                reason=failure["message"].get<std::string>();
            session->state.boot->Delivery(false,reason);
            return result;
        }
        session->state.aiCommandAt = now;
        session->state.bootRestartUsed = true;
        session->state.bootPreset = preset;
        session->state.bootIntervalMs = session->state.boot->Status(now).intervalMs;
        session->state.aiCommandCursor = session->state.aiCursor;
        session->state.aiCommandText = command;
        result["commandId"] = ++session->state.aiCommandNumber;
        session->state.boot->RestartSubmitted(now);
        // Submit the first key now; the local timer/receive path continues it.
        Pulse(session->state, session->connection, now);
        armBootPulse = true;
        result["preset"] = preset;
        result["state"] = session->state.boot->Status(now).state;
        result["intervalMs"] = session->state.boot->Status(now).intervalMs;
        result["untilStopped"] = session->state.boot->Status(now).untilStopped;
        result["entered"] = false;
        return result;
    }
    if (method == "boot_stop") {
        session->state.boot->Cancel();
        return {{"sessionId", id}, {"state", session->state.boot->Status(now).state}, {"stopped", true}};
    }
    if (method == "boot_arm" || method == "boot_status") {
        if (session->mode != 1 && session->mode != 3) return error("U-Boot capture only supports the selected board serial terminal");
        if (method == "boot_arm") {
            if (!session->state.boot->Arm(params.value("pattern", std::string("Hit any key")),
                params.value("interrupt", std::string(" ")), params.value("prompt", std::string("=>")),
                params.value("timeoutSeconds", 90u), now))
                return error("Use bounded printable ASCII boot patterns and a 1..120 second capture window");
            armBootPulse = true;
        }
        const auto state = session->state.boot->Status(now);
        return {{"sessionId", id}, {"state", state.state}, {"matched", state.matched}, {"entered", state.entered},
            {"submittedBursts", state.submitted}, {"remainingMs", state.remainingMs}, {"error", state.error},
            {"pattern", state.pattern}, {"prompt", state.prompt}, {"execution", state.entered ? "prompt-observed" : "unconfirmed"},
            {"source", "local-receive-path"}, {"preset", state.preset}, {"intervalMs", state.intervalMs}, {"untilStopped", state.untilStopped},
            {"maximumBursts", state.preset.empty() ? Json(3) : Json(nullptr)}};
    }
    if (method == "recover") {
        const auto binding = context.recoveryBinding;
        if ((session->mode != 1 && session->mode != 3) || !binding.value("enabled", false) ||
            binding.value("sessionId", std::string()) != id)
            return error("Enable recovery for this exact board terminal and checked power channels in the power page first");
        const auto purpose = params.value("purpose", std::string("unresponsive"));
        if (purpose == "enterUboot") {
            if (session->state.boot->Status(now).state != "armed") return error("Arm local U-Boot capture before a boot recovery cycle");
        } else if (purpose == "unresponsive") {
            if (!session->state.aiCommandAt || now - session->state.aiCommandAt < 5000 || session->state.aiObservedAt < session->state.aiCommandAt ||
                (session->state.aiLastReceivedAt && now - session->state.aiLastReceivedAt < 5000))
                return error("No current inactivity evidence: read/observe the command and distinguish long work or interaction first");
            const auto line = Trim(session->terminal.CurrentLineText());
            if (session->state.aiCursor > session->state.aiCommandCursor && !line.empty() &&
                (line.back() == L'#' || line.back() == L'$' || line.back() == L'>'))
                return error("A prompt is visible; inactivity alone is not a hang");
            auto command = session->state.aiCommandText;
            std::transform(command.begin(), command.end(), command.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            for (const char *operation : {"flash", "erase", "mkfs", "fsck", "dd ", "nand write", "mmc write"})
                if (command.find(operation) != std::string::npos)
                    return error("Potentially non-interruptible operation; automatic power recovery refused");
        } else return error("Unknown recovery purpose");
        if (!session->state.recoveryAction.empty()) return error("A recovery was already submitted in this task; query its original action");
        GUID guid{}; wchar_t unique[40]{};
        if (FAILED(CoCreateGuid(&guid)) || !StringFromGUID2(guid, unique, 40)) return error("Cannot allocate recovery identity");
        const auto result = power_.Submit({{"type", "recover"}, {"source", "ai:desktop"},
            {"sessionId", id}, {"channels", binding.at("channels")}, {"identity", binding.at("identity")},
            {"mode", binding.at("mode")}, {"requestId", "ai:recover:" + WideToMultiByte(unique, CP_UTF8)}});
        if (!result.contains("error")) session->state.recoveryAction = result.value("id", std::string());
        return result;
    }
    if (method == "screen") {
        session->state.aiObservedAt = now;
        auto text = WideToMultiByte(session->terminal.ScreenText(), CP_UTF8);
        bool truncated = text.size() > 65536 || session->terminal.Rows() > 200;
        if (text.size() > 65536) {
            size_t end = 65536;
            while (end && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) --end;
            text.resize(end);
        }
        return {{"sessionId", id}, {"text", text}, {"source", "current-vt-screen"}, {"truncated", truncated},
            {"columns", session->terminal.Columns()}, {"rows", session->terminal.Rows()},
            {"cursorRow", session->terminal.CursorRow()}, {"cursorColumn", session->terminal.CursorColumn()},
            {"alternateScreen", session->terminal.AlternateScreen()}, {"keyboardFlags", session->terminal.KeyboardFlags()},
            {"mouseCaptured", session->terminal.MouseCaptured()}, {"cursor", session->state.aiCursor}};
    }
    if (method == "key") {
        if (context.macroRunning || session->mode == 4)
            return error("Named keys require an idle interactive serial, Telnet or SSH terminal; CMD uses complete lines");
        const auto bytes = session->terminal.KeySequence(params.value("key", std::string()));
        if (bytes.empty()) return error("Unsupported key; use Enter, Ctrl+C, Escape, Tab, arrows or function keys");
        return Send(*session, Bytes(bytes.begin(), bytes.end()));
    }
    if (method == "read") {
        session->state.aiObservedAt = now;
        const auto after = params.value("after", std::uint64_t(0));
        const int lines = std::clamp(params.value("lines", 200), 1, 500);
        std::string text, first, last;
        for (const auto& chunk : session->state.aiOutput) if (!after || chunk.seq > after) {
            if (first.empty()) first = WideToMultiByte(chunk.timestamp, CP_UTF8);
            last = WideToMultiByte(chunk.timestamp, CP_UTF8); text += chunk.text;
        }
        bool limited = false;
        int count = 0;
        for (size_t i = text.size(); i > 0; --i) if (text[i - 1] == '\n' && i < text.size() && ++count >= lines) {
            text.erase(0, i); limited = true; break;
        }
        const bool gap = after && (after > session->state.aiCursor ||
            (!session->state.aiOutput.empty() && after + 1 < session->state.aiOutput.front().seq));
        return {{"sessionId", id}, {"name", WideToMultiByte(session->name, CP_UTF8)},
            {"text", text}, {"source", "device-output"}, {"kind", ai::TerminalKind(session->mode)},
            {"encoding", "UTF-8"}, {"sourceCodePage", session->connection->OutputCodePage() ? session->connection->OutputCodePage() : session->codePage},
            {"firstReceivedAt", first}, {"lastReceivedAt", last}, {"timestampBasis", "local-decoded-receive-time"},
            {"cursor", session->state.aiCursor}, {"gap", gap}, {"truncated", limited || session->state.aiTruncated},
            {"transportNotice", session->state.aiTransportNotice},
            {"maximumBytes", 65536}, {"maximumLines", lines}, {"bytes", text.size()},
            {"lastOutputAgeMs", session->state.aiLastReceivedAt ? Json(now - session->state.aiLastReceivedAt) : Json(nullptr)},
            {"commandId", session->state.aiCommandNumber},
            {"commandAgeMs", session->state.aiCommandAt ? Json(now - session->state.aiCommandAt) : Json(nullptr)},
            {"outputSinceCommand", session->state.aiCursor > session->state.aiCommandCursor},
            {"execution", "unknown"}, {"noOutputIsNotHangProof", true}};
    }
    if (method == "command" || method == "text") {
        if (context.macroRunning) return error("A command macro is in progress; stop it before sending an AI command");
        const auto command = params.value(method == "command" ? "command" : "text", std::string());
        if (method == "command" ? !ai::ValidCommand(command) :
            command.empty() || command.size() > 4096 || std::any_of(command.begin(),command.end(),[](unsigned char c) {
                return (c < 32 && c != '\r' && c != '\n' && c != '\t') || c == 127;
            }))
            return error("Invalid bounded terminal input");
        if (method == "text" && session->mode == 4) return error("CMD accepts complete command lines only");
        // Never use the current visible session: the explicit ID was pinned at
        // turn start and displayed in the tool message before this callback.
        const auto wide = MultiByteToWide(reinterpret_cast<const uint8_t*>(command.data()), command.size(), CP_UTF8);
        const wchar_t* endings[] = {L"\r", L"\n", L"\r\n", L""};
        auto outgoing = wide;
        if (method == "text" && session->terminal.BracketedPaste()) outgoing = L"\x1b[200~" + outgoing + L"\x1b[201~";
        if (method == "command" && session->mode != 0)
            outgoing += endings[session->mode == 4 ? 2 : std::clamp(session->lineEndingIndex, 0, 3)];
        const UINT selectedPage = session->connection->InputCodePage() ? session->connection->InputCodePage() : session->codePage;
        const UINT page = selectedPage == 20936 && !IsValidCodePage(20936) ? 936 : selectedPage;
        BOOL replacement = FALSE;
        auto used = page == CP_UTF8 ? nullptr : &replacement;
        const DWORD flags = page == CP_UTF8 ? WC_ERR_INVALID_CHARS : WC_NO_BEST_FIT_CHARS;
        const int bytes = WideCharToMultiByte(page, flags, outgoing.data(), static_cast<int>(outgoing.size()), nullptr, 0, nullptr, used);
        if (bytes <= 0 || replacement) return error("The selected terminal encoding cannot represent this command exactly");
        std::string encoded(bytes, '\0');
        WideCharToMultiByte(page, flags, outgoing.data(), static_cast<int>(outgoing.size()), encoded.data(), bytes, nullptr, used);
        if (replacement) return error("Terminal command encoding would replace characters; nothing was submitted");
        if (method == "command" && session->mode == 0) encoded += session->terminal.KeySequence("Enter");
        auto result = Send(*session, Bytes(encoded.begin(), encoded.end()));
        if (!result.contains("error") && method == "command") {
            session->state.aiCommandAt = now;
            session->state.aiCommandCursor = session->state.aiCursor;
            session->state.aiCommandText = command;
            result["commandId"] = ++session->state.aiCommandNumber;
            result["cursorBeforeSend"] = session->state.aiCursor;
        }
        return result;
    }

    return error("Unknown bounded SerialCtl operation");
}
void AiDeviceBridge::Observe(State& state, const std::wstring& decoded,
                             const std::wstring& timestamp, ULONGLONG now) {
    if (decoded.empty()) return;
    state.aiLastReceivedAt = now;
    // Bound the additional AI copy before conversion even if the terminal
    // received a large coalesced batch. Serial receive never waits on AI.
    size_t first = decoded.size() > 32768 ? decoded.size() - 32768 : 0;
    if (first && decoded[first] >= 0xdc00 && decoded[first] <= 0xdfff) ++first;
    if (first) state.aiTruncated = true;
    auto output = WideToMultiByte(decoded.substr(first), CP_UTF8);
    if (output.size() > 65536) {
        size_t start = output.size() - 65536;
        while (start < output.size() && (static_cast<unsigned char>(output[start]) & 0xc0) == 0x80) ++start;
        output.erase(0, start); state.aiTruncated = true;
    }
    state.aiBytes += output.size();
    state.aiOutput.push_back({++state.aiCursor, timestamp, std::move(output)});
    while (state.aiBytes > 65536 || state.aiOutput.size() > 256) {
        state.aiBytes -= state.aiOutput.front().text.size(); state.aiOutput.pop_front(); state.aiTruncated = true;
    }
}

bool AiDeviceBridge::Pulse(State& state, const std::shared_ptr<IConnection>& connection, ULONGLONG now) {
    const auto status = state.boot->Status(now);
    const bool active = status.state == "armed" || status.state == "interrupting" || status.state == "prepared";
    const auto bytes = state.boot->Poll(now);
    if (!bytes.empty()) {
        std::wstring error;
        const bool accepted = connection && connection->IsConnected() && connection->Send(Bytes(bytes.begin(), bytes.end()), error);
        state.boot->Delivery(accepted, WideToMultiByte(error, CP_UTF8));
    }
    return active;
}

} // namespace serialctl
