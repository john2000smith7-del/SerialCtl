#include "WorkspaceInstructions.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

using namespace serialctl::ai;

namespace {
void Check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
}

int main(int argc, char** argv) {
    // Exercise the exact production POSIX command on the remote-side OS.
    if (argc == 2 && std::string(argv[1]) == "--read-script") {
        std::cout << WorkspaceReadCommand();
        return 0;
    }
    if (argc == 6 && std::string(argv[1]) == "--replace-script") {
        std::cout << WorkspaceReplaceCommand(argv[2], std::string(argv[3]) == "1",
                                             argv[4], std::string(argv[5]) == "1");
        return 0;
    }
    try {
        const std::vector<std::string> blocked = {
            "API_KEY = test-placeholder",
            "api-key: placeholder",
            "PASSWORD : placeholder",
            "password placeholder",
            "TOKEN = placeholder",
            "Authorization: Bearer placeholder",
            "SSH 密码 123456",
            "登录口令 123456",
            "Bearer example-token-value",
            "https://user:placeholder@example.com/build",
            "-----BEGIN PRIVATE KEY-----",
            "运行 sshpass -p example ssh host",
            "sample key\n- unauthorized second practice",
            std::string("foo") + static_cast<char>(1) + "bar"
        };
        for (const auto& item : blocked) {
            Check(SanitizePracticeEntry(item).empty(),
                  "credential or control-character entry must be rejected");
        }
        const std::vector<std::string> safe = {
            "编译前进入 docker build 容器",
            "板卡 SSH 地址 192.168.1.10 用户名 developer",
            "运行 make -j8 并检查返回码为 0",
            "使用 passwordless SSH 已验证有效",
            "部署后验证固件 SHA256 与构建产物一致"
        };
        for (const auto& item : safe) {
            Check(!SanitizePracticeEntry(item).empty(),
                  "nonsecret engineering practice should be retained");
        }

        const std::string original = "# User instructions\nDo not edit this section.\n";
        const std::string merged = MergeManagedBlock(original,
            {"编译：make -j8", "API_KEY = test-placeholder", "登录密码 123456"});
        Check(merged.rfind(original, 0) == 0, "unmanaged AGENTS.md section must remain untouched");
        Check(merged.find("编译：make -j8") != std::string::npos,
              "validated safe practice must be written");
        Check(merged.find("test-placeholder") == std::string::npos &&
              merged.find("123456") == std::string::npos,
              "sensitive practice must not be persisted");
        Check(BuildTurnContextPrefix(merged).find("编译：make -j8") != std::string::npos,
              "only clean managed practices should be loaded for AI");
        Check(BuildTurnContextPrefix(merged).find("123456") == std::string::npos,
              "sensitive practice must not enter prompt context");
        std::cout << "PASS: workspace practice redaction and managed-block preservation\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        return 1;
    }
}
