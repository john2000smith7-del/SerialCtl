#include "McpProtocol.h"

namespace serialctl::mcp {

const std::vector<McpToolDefinition>& McpProtocol::ToolDefinitions() {
    static const std::vector<McpToolDefinition> s_tools = {
        // --- App & Lifecycle ---
        {"app.status", "Get SerialCtl application and service status", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"app.health", "Check health of SerialCtl background MCP service", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"app.launch", "Launch or activate the single SerialCtl GUI instance", {
            {"type", "object"}, {"properties", {
                {"forceFocus", {{"type", "boolean"}, {"description", "Bring window to front"}}}
            }}
        }},
        {"app.activate", "Activate and bring existing SerialCtl GUI window to front", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"app.quit", "Close and exit SerialCtl GUI (requires confirm=true)", {
            {"type", "object"}, {"properties", {
                {"confirm", {{"type", "boolean"}, {"description", "Explicit confirmation to quit"}}}
            }}, {"required", {"confirm"}}
        }},
        {"app.service_status", "Query resident MCP background service configuration and state", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"app.service_configure", "Configure resident MCP background service settings", {
            {"type", "object"}, {"properties", {
                {"autoStart", {{"type", "boolean"}, {"description", "Enable auto-start on Windows boot/login"}}},
                {"port", {{"type", "integer"}, {"description", "MCP HTTP port (default 7080)"}}}
            }}
        }},

        // --- Session & Connection ---
        {"session.list", "List all active connection sessions in SerialCtl", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"session.ports", "Enumerate local COM serial ports and shared serial instances", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"session.connect_serial", "Open a local COM serial port connection", {
            {"type", "object"}, {"properties", {
                {"port", {{"type", "string"}, {"description", "COM port name, e.g. COM1"}}},
                {"baudRate", {{"type", "integer"}, {"description", "Baud rate, e.g. 115200"}}},
                {"dataBits", {{"type", "integer"}, {"description", "Data bits (5, 6, 7, 8)"}}},
                {"parity", {{"type", "integer"}, {"description", "Parity (0=None, 1=Odd, 2=Even)"}}},
                {"stopBits", {{"type", "integer"}, {"description", "Stop bits (0=1, 1=1.5, 2=2)"}}}
            }}, {"required", {"port"}}
        }},
        {"session.connect_shared_serial", "Connect to a remote shared serial port instance", {
            {"type", "object"}, {"properties", {
                {"host", {{"type", "string"}, {"description", "Remote host/IP"}}},
                {"port", {{"type", "integer"}, {"description", "Remote service port"}}},
                {"remoteCom", {{"type", "string"}, {"description", "Target remote COM port"}}},
                {"instance", {{"type", "string"}, {"description", "Optional shared instance identifier"}}}
            }}, {"required", {"host", "port", "remoteCom"}}
        }},
        {"session.connect_ssh", "Open an SSH terminal connection (host key fingerprint confirmed)", {
            {"type", "object"}, {"properties", {
                {"host", {{"type", "string"}, {"description", "SSH host address"}}},
                {"port", {{"type", "integer"}, {"description", "SSH port (default 22)"}}},
                {"username", {{"type", "string"}, {"description", "SSH username"}}},
                {"password", {{"type", "string"}, {"description", "SSH password"}}},
                {"confirmHostKey", {{"type", "boolean"}, {"description", "Explicit confirmation if host key is untrusted"}}}
            }}, {"required", {"host", "username"}}
        }},
        {"session.connect_telnet", "Open a Telnet connection", {
            {"type", "object"}, {"properties", {
                {"host", {{"type", "string"}, {"description", "Telnet host address"}}},
                {"port", {{"type", "integer"}, {"description", "Telnet port (default 23)"}}}
            }}, {"required", {"host"}}
        }},
        {"session.connect_cmd", "Open a local Windows Command Prompt (CMD) session", {
            {"type", "object"}, {"properties", {
                {"command", {{"type", "string"}, {"description", "Initial command or empty for interactive shell"}}}
            }}
        }},
        {"session.switch", "Switch active session tab in GUI", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID, e.g. session-1"}}}
            }}, {"required", {"sessionId"}}
        }},
        {"session.reconnect", "Reconnect an existing disconnected session", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID, e.g. session-1"}}}
            }}, {"required", {"sessionId"}}
        }},
        {"session.disconnect", "Disconnect an active connection session", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID, e.g. session-1"}}}
            }}, {"required", {"sessionId"}}
        }},
        {"session.close", "Close and remove a session card", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID, e.g. session-1"}}}
            }}, {"required", {"sessionId"}}
        }},
        {"session.disconnect_all", "Disconnect all active sessions", {
            {"type", "object"}, {"properties", Json::object()}
        }},

        // --- SSH History ---
        {"ssh_history.list", "List saved SSH connection history (passwords strictly masked)", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"ssh_history.delete", "Delete an entry from SSH connection history", {
            {"type", "object"}, {"properties", {
                {"host", {{"type", "string"}, {"description", "Host address"}}},
                {"username", {{"type", "string"}, {"description", "Username"}}},
                {"port", {{"type", "integer"}, {"description", "Port"}}}
            }}, {"required", {"host", "username"}}
        }},

        // --- Terminal I/O & Boot ---
        {"terminal.screen", "Get current virtual terminal screen buffer and dimensions", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID"}}}
            }}, {"required", {"sessionId"}}
        }},
        {"terminal.read", "Read received output chunks from terminal session", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID"}}},
                {"after", {{"type", "integer"}, {"description", "Read chunks strictly after this sequence cursor"}}},
                {"lines", {{"type", "integer"}, {"description", "Maximum lines to read (default 200)"}}}
            }}, {"required", {"sessionId"}}
        }},
        {"terminal.send_text", "Send text string to terminal", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID"}}},
                {"text", {{"type", "string"}, {"description", "Text to send"}}}
            }}, {"required", {"sessionId", "text"}}
        }},
        {"terminal.send_bytes", "Send raw hex bytes to terminal session", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID"}}},
                {"hex", {{"type", "string"}, {"description", "Hexadecimal byte string, e.g. '010203'"}}}
            }}, {"required", {"sessionId", "hex"}}
        }},
        {"terminal.send_key", "Send special key or control sequence (Enter, Ctrl+C, Tab, Escape, etc.)", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID"}}},
                {"key", {{"type", "string"}, {"description", "Key name, e.g. Enter, Ctrl+C, Tab, Escape"}}}
            }}, {"required", {"sessionId", "key"}}
        }},
        {"terminal.command", "Send command line and track its execution ID", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID"}}},
                {"command", {{"type", "string"}, {"description", "Command line to send"}}}
            }}, {"required", {"sessionId", "command"}}
        }},
        {"terminal.observe", "Observe terminal activity, cursor, and command execution state", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID"}}}
            }}, {"required", {"sessionId"}}
        }},
        {"terminal.clear", "Clear terminal screen buffer and reset scrollback", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID"}}}
            }}, {"required", {"sessionId"}}
        }},
        {"terminal.settings", "Query or update terminal settings (encoding, CRLF, echo, timestamps, raw trace)", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID"}}},
                {"codePage", {{"type", "integer"}, {"description", "Code page (65001 for UTF-8, 936 for GBK)"}}},
                {"lineEnding", {{"type", "integer"}, {"description", "Line ending: 0=CR, 1=LF, 2=CRLF, 3=None"}}},
                {"echo", {{"type", "boolean"}, {"description", "Local echo enabled"}}},
                {"timestamp", {{"type", "boolean"}, {"description", "Timestamp enabled"}}},
                {"rawTrace", {{"type", "boolean"}, {"description", "Raw trace logging enabled"}}}
            }}, {"required", {"sessionId"}}
        }},

        // --- U-Boot Interrupt ---
        {"boot.enter", "Trigger U-Boot / bootloader interrupt sequence", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID"}}},
                {"preset", {{"type", "string"}, {"description", "Boot preset name, e.g. 'uboot'"}}},
                {"timeoutSeconds", {{"type", "integer"}, {"description", "Timeout in seconds (1..120)"}}}
            }}, {"required", {"sessionId"}}
        }},
        {"boot.stop", "Stop active U-Boot interrupt polling sequence", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID"}}}
            }}, {"required", {"sessionId"}}
        }},
        {"boot.status", "Query status of U-Boot interrupt matching", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID"}}}
            }}, {"required", {"sessionId"}}
        }},

        // --- Macro & Command Notebook ---
        {"macro.list", "List all configured command macros", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"macro.set_document", "Set command notebook document text", {
            {"type", "object"}, {"properties", {
                {"text", {{"type", "string"}, {"description", "Notebook text"}}}
            }}, {"required", {"text"}}
        }},
        {"macro.add", "Add a new macro step", {
            {"type", "object"}, {"properties", {
                {"name", {{"type", "string"}, {"description", "Macro step title"}}},
                {"commands", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "List of commands"}}},
                {"delayMs", {{"type", "integer"}, {"description", "Delay in milliseconds between commands"}}}
            }}, {"required", {"name", "commands"}}
        }},
        {"macro.update", "Update an existing macro step", {
            {"type", "object"}, {"properties", {
                {"index", {{"type", "integer"}, {"description", "Step index"}}},
                {"name", {{"type", "string"}, {"description", "Macro step title"}}},
                {"commands", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "List of commands"}}},
                {"delayMs", {{"type", "integer"}, {"description", "Delay in milliseconds"}}}
            }}, {"required", {"index", "name", "commands"}}
        }},
        {"macro.delete", "Delete a macro step by index", {
            {"type", "object"}, {"properties", {
                {"index", {{"type", "integer"}, {"description", "Step index to remove"}}}
            }}, {"required", {"index"}}
        }},
        {"macro.run", "Run macro or notebook commands on target session", {
            {"type", "object"}, {"properties", {
                {"index", {{"type", "integer"}, {"description", "Macro step index or omitted to run notebook"}}},
                {"sessionId", {{"type", "string"}, {"description", "Target session ID"}}}
            }}
        }},
        {"macro.stop", "Stop running command macro sequence", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"macro.status", "Query current running status of command macro", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"macro.import_file", "Import commands from a local file", {
            {"type", "object"}, {"properties", {
                {"path", {{"type", "string"}, {"description", "File path to import"}}}
            }}, {"required", {"path"}}
        }},
        {"macro.export_file", "Export commands to a local file", {
            {"type", "object"}, {"properties", {
                {"path", {{"type", "string"}, {"description", "File path to export"}}}
            }}, {"required", {"path"}}
        }},

        // --- SFTP ---
        {"sftp.list", "List files in remote directory", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Optional SSH session ID"}}},
                {"path", {{"type", "string"}, {"description", "Remote directory path"}}}
            }}
        }},
        {"sftp.cd", "Change remote directory", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Optional SSH session ID"}}},
                {"path", {{"type", "string"}, {"description", "Target remote directory"}}}
            }}, {"required", {"path"}}
        }},
        {"sftp.mkdir", "Create remote directory", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Optional SSH session ID"}}},
                {"remotePath", {{"type", "string"}, {"description", "Directory path to create"}}}
            }}, {"required", {"remotePath"}}
        }},
        {"sftp.rename", "Rename remote file or directory", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Optional SSH session ID"}}},
                {"oldPath", {{"type", "string"}, {"description", "Source remote path"}}},
                {"newPath", {{"type", "string"}, {"description", "Destination remote path"}}}
            }}, {"required", {"oldPath", "newPath"}}
        }},
        {"sftp.delete", "Delete remote file or directory (requires confirm=true)", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Optional SSH session ID"}}},
                {"remotePath", {{"type", "string"}, {"description", "Target path to delete"}}},
                {"confirm", {{"type", "boolean"}, {"description", "Explicit confirmation"}}}
            }}, {"required", {"remotePath", "confirm"}}
        }},
        {"sftp.chmod", "Change remote file permissions", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Optional SSH session ID"}}},
                {"remotePath", {{"type", "string"}, {"description", "Target file path"}}},
                {"mode", {{"type", "integer"}, {"description", "Octal file mode, e.g. 0755"}}}
            }}, {"required", {"remotePath", "mode"}}
        }},
        {"sftp.upload", "Upload local file to remote server", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Optional SSH session ID"}}},
                {"localPath", {{"type", "string"}, {"description", "Local file path"}}},
                {"remotePath", {{"type", "string"}, {"description", "Remote destination path"}}},
                {"allowOverwrite", {{"type", "boolean"}, {"description", "Allow overwrite"}}}
            }}, {"required", {"localPath", "remotePath"}}
        }},
        {"sftp.download", "Download remote file to local computer", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Optional SSH session ID"}}},
                {"remotePath", {{"type", "string"}, {"description", "Remote source path"}}},
                {"localPath", {{"type", "string"}, {"description", "Local destination path"}}},
                {"allowOverwrite", {{"type", "boolean"}, {"description", "Allow overwrite"}}}
            }}, {"required", {"remotePath", "localPath"}}
        }},
        {"sftp.transfers", "List active SFTP transfer jobs", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"sftp.cancel_transfer", "Cancel an SFTP transfer job", {
            {"type", "object"}, {"properties", {
                {"transferId", {{"type", "integer"}, {"description", "Transfer job ID"}}}
            }}, {"required", {"transferId"}}
        }},
        {"sftp.retry_transfer", "Retry a failed SFTP transfer job", {
            {"type", "object"}, {"properties", {
                {"transferId", {{"type", "integer"}, {"description", "Transfer job ID"}}}
            }}, {"required", {"transferId"}}
        }},
        {"sftp.clear_transfers", "Clear completed SFTP transfers from queue", {
            {"type", "object"}, {"properties", Json::object()}
        }},

        // --- Xfer ---
        {"xfer.status", "Query serial file transfer status", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"xfer.start", "Start serial file transfer (raw, xmodem, ymodem, zmodem)", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Serial session ID"}}},
                {"protocol", {{"type", "string"}, {"enum", {"raw", "xmodem", "ymodem", "zmodem"}}}},
                {"direction", {{"type", "string"}, {"enum", {"send", "receive"}}}},
                {"filePath", {{"type", "string"}, {"description", "Local file path"}}},
                {"expectedSize", {{"type", "integer"}, {"description", "Optional expected size in bytes"}}}
            }}, {"required", {"sessionId", "protocol", "direction", "filePath"}}
        }},
        {"xfer.finish_raw", "Finish raw serial receive mode", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"xfer.cancel", "Cancel active serial file transfer", {
            {"type", "object"}, {"properties", Json::object()}
        }},

        // --- TFTP ---
        {"tftp.status", "Query TFTP service status and local published items", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"tftp.start", "Start UDP TFTP server", {
            {"type", "object"}, {"properties", {
                {"bindIp", {{"type", "string"}, {"description", "IP to bind, e.g. 0.0.0.0"}}},
                {"port", {{"type", "integer"}, {"description", "UDP port (default 69)"}}}
            }}
        }},
        {"tftp.stop", "Stop UDP TFTP server", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"tftp.publish_local", "Publish a local file via TFTP", {
            {"type", "object"}, {"properties", {
                {"tftpName", {{"type", "string"}, {"description", "Virtual TFTP filename"}}},
                {"localPath", {{"type", "string"}, {"description", "Local absolute file path"}}},
                {"allowOverwrite", {{"type", "boolean"}, {"description", "Allow overwrite"}}}
            }}, {"required", {"tftpName", "localPath"}}
        }},
        {"tftp.publish_remote", "Publish a remote file streamed via SSH/SFTP (zero disk footprint)", {
            {"type", "object"}, {"properties", {
                {"tftpName", {{"type", "string"}, {"description", "Virtual TFTP filename"}}},
                {"remotePath", {{"type", "string"}, {"description", "Remote file path on SSH server"}}},
                {"sessionId", {{"type", "string"}, {"description", "Optional SSH session ID"}}},
                {"allowOverwrite", {{"type", "boolean"}, {"description", "Allow overwrite"}}}
            }}, {"required", {"tftpName", "remotePath"}}
        }},
        {"tftp.unpublish", "Unpublish a file from TFTP server", {
            {"type", "object"}, {"properties", {
                {"tftpName", {{"type", "string"}, {"description", "Virtual TFTP filename to remove"}}}
            }}, {"required", {"tftpName"}}
        }},
        {"tftp.list", "List all currently published TFTP files", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"tftp.transfers", "List active TFTP client transfers and progress", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"tftp.cancel_transfer", "Cancel an active TFTP download transfer", {
            {"type", "object"}, {"properties", {
                {"transferId", {{"type", "integer"}, {"description", "Transfer ID"}}}
            }}, {"required", {"transferId"}}
        }},

        // --- Log & Diagnostics ---
        {"log.status", "Query session logging status", {
            {"type", "object"}, {"properties", {
                {"sessionId", {{"type", "string"}, {"description", "Session ID"}}}
            }}
        }},
        {"log.export", "Export session log to file", {
            {"type", "object"}, {"properties", {
                {"path", {{"type", "string"}, {"description", "Destination file path"}}},
                {"csv", {{"type", "boolean"}, {"description", "Export as CSV format"}}}
            }}, {"required", {"path"}}
        }},
        {"log.cancel_export", "Cancel ongoing log export job", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"log.network_diagnostics", "Retrieve network diagnostics and socket states", {
            {"type", "object"}, {"properties", Json::object()}
        }},

        // --- UI & Appearance ---
        {"ui.status", "Query UI panel layout, dark theme, and active tabs", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"ui.theme", "Switch UI light/dark theme", {
            {"type", "object"}, {"properties", {
                {"dark", {{"type", "boolean"}, {"description", "True for dark theme, false for light theme"}}}
            }}, {"required", {"dark"}}
        }},
        {"ui.panel", "Switch right sidebar tab (commands, sftp, xfer, tftp)", {
            {"type", "object"}, {"properties", {
                {"name", {{"type", "string"}, {"enum", {"commands", "sftp", "xfer", "tftp"}}}}
            }}, {"required", {"name"}}
        }},

        // --- Built-in AI ---
        {"ai.status", "Query built-in AI assistant status", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"ai.workspace", "Get AI workspace instructions and context", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"ai.send", "Send prompt to built-in AI (recursion depth limited)", {
            {"type", "object"}, {"properties", {
                {"prompt", {{"type", "string"}, {"description", "Prompt text"}}},
                {"recursionDepth", {{"type", "integer"}, {"description", "Anti-recursion tracking depth"}}}
            }}, {"required", {"prompt"}}
        }},
        {"ai.stop", "Stop active AI reply generation", {
            {"type", "object"}, {"properties", Json::object()}
        }},

        // --- Controlled Power (Strict Whitelist Only) ---
        {"power.get", "Query power supply status, output state, and currently checked channels", {
            {"type", "object"}, {"properties", Json::object()}
        }},
        {"power.output", "Turn ON/OFF power output ONLY on GUI currently checked channels (requires unlocked Test)", {
            {"type", "object"}, {"properties", {
                {"enabled", {{"type", "boolean"}, {"description", "True to energize, False to turn off"}}},
                {"reason", {{"type", "string"}, {"description", "Audit reason for power action"}}}
            }}, {"required", {"enabled"}}
        }},
        {"power.action", "Query status of an asynchronous power operation by action ID", {
            {"type", "object"}, {"properties", {
                {"id", {{"type", "string"}, {"description", "Action ID"}}}
            }}, {"required", {"id"}}
        }},
        {"power.recover", "Trigger bound power recovery cycle (requires unlocked Test)", {
            {"type", "object"}, {"properties", Json::object()}
        }}
    };
    return s_tools;
}

Json McpProtocol::HandleInitialize(const Json& id) {
    return MakeSuccessResponse(id, {
        {"protocolVersion", "2024-11-05"},
        {"capabilities", {
            {"tools", Json::object()}
        }},
        {"serverInfo", {
            {"name", "serialctl-mcp"},
            {"version", "2.0.20"}
        }}
    });
}

Json McpProtocol::HandlePing(const Json& id) {
    return MakeSuccessResponse(id, Json::object());
}

Json McpProtocol::HandleToolsList(const Json& id) {
    const auto& defs = ToolDefinitions();
    Json toolsArray = Json::array();
    for (const auto& tool : defs) {
        toolsArray.push_back({
            {"name", tool.name},
            {"description", tool.description},
            {"inputSchema", tool.inputSchema}
        });
    }
    return MakeSuccessResponse(id, {
        {"tools", toolsArray}
    });
}

Json McpProtocol::MakeSuccessResponse(const Json& id, const Json& result) {
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"result", result}
    };
}

Json McpProtocol::MakeErrorResponse(const Json& id, int code, const std::string& message, const Json& data) {
    Json err = {
        {"code", code},
        {"message", message}
    };
    if (!data.is_null()) {
        err["data"] = data;
    }
    return {
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", err}
    };
}

Json McpProtocol::MakeToolResult(const Json& contentObj, bool isError) {
    Json arr = Json::array();
    arr.push_back({
        {"type", "text"},
        {"text", contentObj.dump()}
    });
    return {
        {"content", arr},
        {"isError", isError}
    };
}

Json McpProtocol::MakeToolTextResult(const std::string& text, bool isError) {
    Json arr = Json::array();
    arr.push_back({
        {"type", "text"},
        {"text", text}
    });
    return {
        {"content", arr},
        {"isError", isError}
    };
}

} // namespace serialctl::mcp
