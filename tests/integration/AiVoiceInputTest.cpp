#include <windows.h>
#include "AiVoiceInput.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace serialctl::ai {
struct VoiceInputTestAccess {
    static bool Start(VoiceInput &voice, std::wstring &error, VoiceInput::Capture capture) {
        return voice.StartCapture(error, capture);
    }
};
}

namespace {
using namespace serialctl::ai;

void Require(bool ok, const char *reason) {
    if (!ok) throw std::runtime_error(reason);
}

std::vector<std::uint8_t> FixtureWave(std::uint32_t size) {
    const std::uint8_t header[]{
        'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0,
        1,0, 1,0, 0x80,0x3e,0,0, 0x00,0x7d,0,0, 2,0, 16,0, 'd','a','t','a', 0,0,0,0
    };
    std::vector<std::uint8_t> wav(std::begin(header), std::end(header));
    for (unsigned i = 0; i < 4; ++i) {
        wav[4 + i] = static_cast<std::uint8_t>((36 + size) >> (8 * i));
        wav[40 + i] = static_cast<std::uint8_t>(size >> (8 * i));
    }
    wav.resize(sizeof(header) + size);
    return wav;
}

void CheckPreview() {
    static std::atomic_bool extend{false};
    VoiceInput voice;
    std::wstring error;
    const auto capture = [](const std::atomic_bool &finish, const std::atomic_bool &cancel,
                            const std::function<void(const std::vector<std::uint8_t> &, unsigned, unsigned)> &onSamples) {
        std::vector<std::uint8_t> samples(3 * 32000);
        while (!finish && !cancel) {
            if (extend) samples.resize(7 * 32000);
            onSamples(samples, 16000, 1);
            Sleep(10);
        }
        return FixtureWave(8 * 32000);
    };
    const auto waitPreview = [&] {
        const auto started = GetTickCount64();
        while (voice.Status() == VoiceInput::State::Recording && GetTickCount64() - started < 5000) {
            if (auto text = voice.TakePreview()) return *text;
            Sleep(10);
        }
        return std::wstring{};
    };
    const auto waitResult = [&] {
        const auto started = GetTickCount64();
        while (GetTickCount64() - started < 5000) {
            if (auto result = voice.TakeResult()) return *result;
            Sleep(10);
        }
        throw std::runtime_error("Voice worker must complete");
    };
    Require(VoiceInputTestAccess::Start(voice, error, capture), "Start controlled capture");
    Require(waitPreview() == L"首段预览", "Show the first partial transcript before recording stops");
    extend = true;
    Require(waitPreview() == L"更新后的完整预览", "Return a revised preview from the accumulated audio");
    voice.FinishRecording();
    Require(!voice.TakePreview(), "Finalizing must stop publishing previews");
    const auto final = waitResult();
    Require(final.error.empty() && final.text == L"包含录音尾段的最终文字", "Final pass must include the recording tail");
    extend = false;
    Require(VoiceInputTestAccess::Start(voice, error, capture), "Start a fresh controlled capture");
    Require(waitPreview() == L"首段预览", "The second recording must produce its own preview");
    voice.Cancel();
    Require(!voice.TakePreview() && waitResult().text.empty(), "Canceled recording must discard preview and final text");
}

int RunVoiceWorkerFixture(int argc, wchar_t **argv) {
    if (argc != 4 || std::wstring_view(argv[1]) != VoiceWorkerArgument) return 5;
    const std::filesystem::path input(argv[2]), output(argv[3]);
    std::ifstream wave(input, std::ios::binary);
    char header[12]{};
    wave.read(header, sizeof(header));
    if (!wave || std::string(header, 4) != "RIFF" || std::string(header + 8, 4) != "WAVE") return 6;
    std::ofstream result(output, std::ios::binary);
    switch (std::filesystem::file_size(input)) {
    case 44 + 3 * 32000: result << u8"首段预览"; break;
    case 44 + 7 * 32000: result << u8"更新后的完整预览"; break;
    case 44 + 8 * 32000: result << u8"包含录音尾段的最终文字"; break;
    default: result << u8"  检查 COM3 的 U-Boot 日志  \n"; break;
    }
    return result ? 0 : 7;
}
} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc > 1) return RunVoiceWorkerFixture(argc, argv);
    try {
        const auto wav = FixtureWave(4);
        std::atomic_bool cancel{false};
        auto result = TranscribeVoice(wav, cancel);
        Require(result.error.empty() && result.text == L"检查 COM3 的 U-Boot 日志", "Local process should return UTF-8 text");
        CheckPreview();
        cancel = true;
        Require(TranscribeVoice(wav, cancel).text.empty(), "Canceled voice job must not return a draft");
        std::cout << "[PASS] Local process, live preview, complete final pass, cancellation\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
