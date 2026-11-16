#include "AiVoiceInput.h"
#include "resource.h"
#include "whisper.h"

#include <windows.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>

namespace serialctl::ai {
namespace {

std::vector<float> ReadVoiceWave(const wchar_t *path) {
    std::ifstream input(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    constexpr unsigned maxSamples = 60 * 16000;
    if (size < 44 || size > 2 * maxSamples + 4096) return {};
    std::vector<std::uint8_t> bytes(static_cast<size_t>(size));
    input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return {};
    const auto match = [&](size_t offset, const char *tag) { return std::memcmp(bytes.data() + offset, tag, 4) == 0; };
    const auto number = [&](size_t offset, unsigned count) {
        std::uint32_t result = 0;
        for (unsigned i = 0; i < count; ++i) result |= std::uint32_t{bytes[offset + i]} << (8 * i);
        return result;
    };
    if (!match(0, "RIFF") || !match(8, "WAVE")) return {};
    bool formatOk = false;
    for (size_t offset = 12; offset + 8 <= bytes.size();) {
        const auto length = number(offset + 4, 4);
        const auto data = offset + 8;
        if (length > bytes.size() - data) return {};
        if (match(offset, "fmt ") && length >= 16) {
            formatOk = number(data, 2) == 1 && number(data + 2, 2) == 1 &&
                       number(data + 4, 4) == 16000 && number(data + 14, 2) == 16;
        } else if (match(offset, "data")) {
            if (!formatOk || !length || length % 2 || length / 2 > maxSamples) return {};
            std::vector<float> samples(length / 2);
            for (size_t i = 0; i < samples.size(); ++i) {
                const auto pcm = static_cast<int>(number(data + i * 2, 2));
                samples[i] = static_cast<float>(pcm < 32768 ? pcm : pcm - 65536) / 32768.0f;
            }
            return samples;
        }
        offset = data + length + (length % 2);
    }
    return {};
}

} // namespace

int RunVoiceWorker(const wchar_t *input, const wchar_t *output) {
    try {
        const auto samples = ReadVoiceWave(input);
        if (samples.empty()) return 2;
        const auto module = GetModuleHandleW(nullptr);
        const auto model = FindResourceW(module, MAKEINTRESOURCEW(IDR_VOICE_MODEL), RT_RCDATA);
        if (!model) return 3;
        const auto bytes = SizeofResource(module, model);
        const auto data = LockResource(LoadResource(module, model));
        if (!data || !bytes) return 3;
        auto settings = whisper_context_default_params();
        settings.use_gpu = false;
        whisper_log_set([](ggml_log_level, const char *, void *) {}, nullptr);
        std::unique_ptr<whisper_context, decltype(&whisper_free)> context(
            whisper_init_from_buffer_with_params(data, bytes, settings), whisper_free);
        if (!context) return 3;
        auto params = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        params.n_threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));
        params.language = "auto";
        params.no_timestamps = true;
        params.no_context = true;
        params.print_progress = params.print_realtime = params.print_timestamps = params.print_special = false;
        if (whisper_full(context.get(), params, samples.data(), static_cast<int>(samples.size())) != 0) return 4;
        std::ofstream transcript(std::filesystem::path(output), std::ios::binary);
        for (int i = 0; i < whisper_full_n_segments(context.get()); ++i)
            transcript << whisper_full_get_segment_text(context.get(), i);
        transcript.close();
        return transcript ? 0 : 5;
    } catch (const std::exception &) {
        return 5;
    }
}

} // namespace serialctl::ai
