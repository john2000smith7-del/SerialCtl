#include "AiVoiceInput.h"
#include "Win32Helpers.h"
#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <stdexcept>

namespace serialctl::ai {
namespace {
using Audio = std::vector<std::uint8_t>;
constexpr DWORD MaxRecordingMs = 60000, LocalMs = 120000;
constexpr size_t FirstPreviewMs = 3000, PreviewIntervalMs = 4000;
constexpr size_t MaxText = 256 * 1024;

struct VoiceFailure { std::wstring message; };
[[noreturn]] void Fail(std::wstring message) { throw VoiceFailure{std::move(message)}; }
void CheckCancel(const std::atomic_bool &cancel) {
    if (cancel.load()) Fail(L"语音输入已取消");
}
struct Handle {
    HANDLE value;
    explicit Handle(HANDLE handle) : value(handle) {}
    ~Handle() { if (value) CloseHandle(value); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
};
bool LocalPath(const std::filesystem::path &path) {
    if (!path.is_absolute()) return false;
    const auto root = path.root_path().wstring();
    if (root.size() != 3 || root[1] != L':') return false;
    const auto type = GetDriveTypeW(root.c_str());
    return type == DRIVE_FIXED || type == DRIVE_REMOVABLE || type == DRIVE_CDROM || type == DRIVE_RAMDISK;
}

void Append(Audio &bytes, const std::string &text) {
    bytes.insert(bytes.end(), text.begin(), text.end());
}
void AppendNumber(Audio &bytes, DWORD value, unsigned count) {
    for (unsigned i = 0; i < count; ++i) bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}

Audio ToWave(const Audio &samples, unsigned rate, unsigned channels) {
    const auto frames = samples.size() / (channels * 2);
    const auto output = frames * 16000 / rate;
    if (!output) Fail(L"没有录到音频，请确认麦克风后重试");
    Audio wav;
    wav.reserve(44 + output * 2);
    Append(wav, "RIFF");
    AppendNumber(wav, static_cast<DWORD>(36 + output * 2), 4);
    Append(wav, "WAVEfmt ");
    AppendNumber(wav, 16, 4);
    AppendNumber(wav, 1, 2);
    AppendNumber(wav, 1, 2);
    AppendNumber(wav, 16000, 4);
    AppendNumber(wav, 32000, 4);
    AppendNumber(wav, 2, 2);
    AppendNumber(wav, 16, 2);
    Append(wav, "data");
    AppendNumber(wav, static_cast<DWORD>(output * 2), 4);
    if (rate == 16000 && channels == 1) {
        wav.insert(wav.end(), samples.begin(), samples.begin() + output * 2);
        return wav;
    }
    // Average the corresponding source frames while downmixing to mono.
    for (size_t i = 0; i < output; ++i) {
        const auto first = i * rate / 16000, end = (i + 1) * rate / 16000;
        int sum = 0;
        for (size_t j = first * channels * 2; j < end * channels * 2; j += 2)
            sum += static_cast<std::int16_t>(samples[j] | (samples[j + 1] << 8));
        AppendNumber(wav, static_cast<DWORD>(sum / static_cast<int>((end - first) * channels)), 2);
    }
    return wav;
}

template <typename SamplesReady>
Audio Record(const std::atomic_bool &finish, const std::atomic_bool &cancel, const SamplesReady &samplesReady) {
    wchar_t system[MAX_PATH]{};
    const auto length = GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)));
    if (!length || length >= std::size(system)) Fail(L"无法定位 Windows 录音组件");
    const auto modulePath = std::wstring(system) + L"\\winmm.dll";
    struct Module {
        HMODULE handle;
        ~Module() { if (handle) FreeLibrary(handle); }
    } module{LoadLibraryW(modulePath.c_str())};
    if (!module.handle) Fail(L"无法加载 Windows 录音组件");
    const auto open = reinterpret_cast<decltype(&waveInOpen)>(GetProcAddress(module.handle, "waveInOpen"));
    const auto prepare = reinterpret_cast<decltype(&waveInPrepareHeader)>(GetProcAddress(module.handle, "waveInPrepareHeader"));
    const auto add = reinterpret_cast<decltype(&waveInAddBuffer)>(GetProcAddress(module.handle, "waveInAddBuffer"));
    const auto start = reinterpret_cast<decltype(&waveInStart)>(GetProcAddress(module.handle, "waveInStart"));
    const auto reset = reinterpret_cast<decltype(&waveInReset)>(GetProcAddress(module.handle, "waveInReset"));
    const auto unprepare = reinterpret_cast<decltype(&waveInUnprepareHeader)>(GetProcAddress(module.handle, "waveInUnprepareHeader"));
    const auto close = reinterpret_cast<decltype(&waveInClose)>(GetProcAddress(module.handle, "waveInClose"));
    const auto errorText = reinterpret_cast<decltype(&waveInGetErrorTextW)>(GetProcAddress(module.handle, "waveInGetErrorTextW"));
    if (!open || !prepare || !add || !start || !reset || !unprepare || !close || !errorText)
        Fail(L"Windows 录音组件缺少必要接口");
    Handle event(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!event.value) Fail(L"无法初始化麦克风事件");
    struct Recorder {
        decltype(&waveInReset) reset;
        decltype(&waveInUnprepareHeader) unprepare;
        decltype(&waveInClose) close;
        HWAVEIN device = nullptr;
        std::array<WAVEHDR, 4> headers{};
        std::array<std::array<char, 19200>, 4> buffers{};
        size_t prepared = 0;
        ~Recorder() {
            if (!device) return;
            reset(device);
            for (size_t i = 0; i < prepared; ++i)
                unprepare(device, &headers[i], sizeof(WAVEHDR));
            close(device);
        }
    } recorder{reset, unprepare, close};
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.wBitsPerSample = 16;
    MMRESULT opened = MMSYSERR_BADDEVICEID;
    struct Format { DWORD rate; WORD channels; };
    constexpr Format candidates[]{{16000, 1}, {48000, 1}, {44100, 1}, {48000, 2}, {44100, 2}};
    for (const auto &candidate : candidates) {
        CheckCancel(cancel);
        format.nSamplesPerSec = candidate.rate;
        format.nChannels = candidate.channels;
        format.nBlockAlign = static_cast<WORD>(format.nChannels * 2);
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
        opened = open(&recorder.device, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(event.value), 0, CALLBACK_EVENT);
        if (opened != WAVERR_BADFORMAT) break;
    }
    const auto check = [&](MMRESULT status) {
        if (status == MMSYSERR_NOERROR) return;
        wchar_t detail[MAXERRORLENGTH]{};
        errorText(status, detail, static_cast<UINT>(std::size(detail)));
        Fail(L"麦克风录音失败（" + std::to_wstring(status) + L"）" + (detail[0] ? std::wstring(L" · ") + detail : L""));
    };
    check(opened);
    for (size_t i = 0; i < recorder.headers.size(); ++i) {
        auto &header = recorder.headers[i];
        header.lpData = recorder.buffers[i].data();
        header.dwBufferLength = format.nAvgBytesPerSec / 10;
        check(prepare(recorder.device, &header, sizeof(header)));
        ++recorder.prepared;
        check(add(recorder.device, &header, sizeof(header)));
    }
    CheckCancel(cancel);
    check(start(recorder.device));
    const auto started = GetTickCount64();
    const auto limit = static_cast<size_t>(format.nAvgBytesPerSec) * MaxRecordingMs / 1000;
    Audio samples;
    size_t next = 0;
    const auto drain = [&](bool requeue) {
        for (size_t i = 0; i < recorder.headers.size() && samples.size() < limit; ++i) {
            auto &header = recorder.headers[next];
            if (!(header.dwFlags & WHDR_DONE)) break;
            const auto size = std::min<size_t>({header.dwBytesRecorded, header.dwBufferLength, limit - samples.size()});
            const auto *data = reinterpret_cast<const std::uint8_t *>(header.lpData);
            samples.insert(samples.end(), data, data + size);
            next = (next + 1) % recorder.headers.size();
            if (requeue) check(add(recorder.device, &header, sizeof(header)));
        }
    };
    while (!finish.load() && !cancel.load() && GetTickCount64() - started < MaxRecordingMs && samples.size() < limit) {
        if (WaitForSingleObject(event.value, 30) == WAIT_FAILED) Fail(L"等待麦克风数据失败");
        drain(true);
        samplesReady(samples, format.nSamplesPerSec, format.nChannels);
    }
    check(reset(recorder.device));
    CheckCancel(cancel);
    drain(false);
    return ToWave(samples, format.nSamplesPerSec, format.nChannels);
}

std::wstring Decode(std::string text) {
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);
    if (text.empty()) return {};
    if (text.find('\0') != std::string::npos || text.size() > MaxText) Fail(L"转写结果格式无效");
    const auto size = static_cast<int>(text.size());
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, nullptr, 0);
    if (count <= 0) Fail(L"本机转写返回了无效的 UTF-8 文字");
    std::wstring result(count, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), size, result.data(), count);
    const auto start = result.find_first_not_of(L" \r\n\t");
    if (start == std::wstring::npos) return {};
    return result.substr(start, result.find_last_not_of(L" \r\n\t") - start + 1);
}

std::wstring LocalText(const Audio &wav, const std::atomic_bool &cancel,
                       DWORD priority = 0) {
    struct Scratch {
        std::wstring directory, input, output;
        Scratch() {
            wchar_t root[MAX_PATH + 1]{}, name[40]{};
            const auto length = GetTempPathW(static_cast<DWORD>(std::size(root)), root);
            GUID id{};
            if (!length || length >= std::size(root) || !LocalPath(std::filesystem::path(root)) ||
                FAILED(CoCreateGuid(&id)) ||
                !StringFromGUID2(id, name, static_cast<int>(std::size(name)))) Fail(L"无法创建临时录音目录");
            directory = std::wstring(root) + L"SerialCtl-voice-" + name;
            input = directory + L"\\voice.wav";
            output = directory + L"\\result.txt";
            if (!CreateDirectoryW(directory.c_str(), nullptr)) Fail(L"无法创建临时录音目录");
        }
        ~Scratch() {
            DeleteFileW(input.c_str());
            DeleteFileW(output.c_str());
            RemoveDirectoryW(directory.c_str());
        }
    } files;
    {
        std::ofstream stream(std::filesystem::path(files.input), std::ios::binary);
        stream.write(reinterpret_cast<const char *>(wav.data()), static_cast<std::streamsize>(wav.size()));
        stream.close();
        if (!stream) Fail(L"写入临时录音失败");
    }
    const auto quote = QuoteCommandLineArgument;
    std::wstring executable(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    if (!length || length >= executable.size()) Fail(L"无法定位本机转写程序");
    executable.resize(length);
    std::wstring command = quote(executable) + L" " + VoiceWorkerArgument + L" " + quote(files.input) +
        L" " + quote(files.output);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto directory = std::filesystem::path(executable).parent_path().wstring();
    CheckCancel(cancel);
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | priority,
                        nullptr, directory.c_str(), &startup, &process))
        Fail(L"无法启动本机转写（" + std::to_wstring(GetLastError()) + L"）");
    Handle child(process.hProcess), thread(process.hThread);
    const auto started = GetTickCount64();
    DWORD status = WAIT_TIMEOUT;
    while (!cancel.load() && GetTickCount64() - started < LocalMs) {
        status = WaitForSingleObject(child.value, 40);
        if (status != WAIT_TIMEOUT) break;
    }
    if (status != WAIT_OBJECT_0) {
        TerminateProcess(child.value, ERROR_CANCELLED);
        WaitForSingleObject(child.value, INFINITE);
        CheckCancel(cancel);
        Fail(status == WAIT_TIMEOUT ? L"本机转写超时" : L"等待本机转写程序失败");
    }
    CheckCancel(cancel);
    DWORD exitCode = 1;
    if (!GetExitCodeProcess(child.value, &exitCode) || exitCode != 0)
        Fail(L"本机转写失败（退出码 " + std::to_wstring(exitCode) + L"）");
    std::ifstream stream(std::filesystem::path(files.output), std::ios::binary);
    std::string text(MaxText + 1, '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (stream.bad() || (!stream && !stream.eof())) Fail(L"无法读取本机转写结果");
    text.resize(static_cast<size_t>(stream.gcount()));
    return Decode(std::move(text));
}

} // namespace

VoiceResult TranscribeVoice(const Audio &wav, const std::atomic_bool &cancel) {
    try {
        CheckCancel(cancel);
        if (wav.size() <= 44 || wav.size() > 44 + MaxRecordingMs * 32 ||
            !std::equal(wav.begin(), wav.begin() + 4, "RIFF") ||
            !std::equal(wav.begin() + 8, wav.begin() + 12, "WAVE")) Fail(L"录音 WAV 格式无效");
        const auto text = LocalText(wav, cancel);
        CheckCancel(cancel);
        if (text.empty()) return {{}, L"没有识别到文字，请检查麦克风后重试"};
        return {text, {}};
    } catch (const VoiceFailure &failure) {
        return {{}, failure.message};
    } catch (const std::exception &) {
        return {{}, L"语音转写失败"};
    }
}

VoiceInput::~VoiceInput() { Cancel(); Join(); }

bool VoiceInput::Start(std::wstring &error) {
    return StartCapture(error, Record<SamplesReady>);
}

bool VoiceInput::StartCapture(std::wstring &error, Capture capture) {
    if (state_ != State::Idle || worker_.joinable()) return false;
    finish_ = cancel_ = false;
    preview_.reset();
    result_ = {};
    state_ = State::Recording;
    try {
        worker_ = std::thread([this, capture] {
            std::atomic_bool stopPreview{false};
            std::future<std::wstring> pending;
            size_t nextPreviewMs = FirstPreviewMs;
            Audio wav;
            try {
                wav = capture(finish_, cancel_, [&](const Audio &samples, unsigned rate, unsigned channels) {
                    if (finish_ || cancel_) return;
                    if (pending.valid()) {
                        if (pending.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) return;
                        auto text = pending.get();
                        if (!text.empty()) {
                            std::lock_guard<std::mutex> lock(previewMutex_);
                            preview_ = std::move(text);
                        }
                    }
                    const auto recordedMs = samples.size() * 1000 / (rate * channels * 2);
                    if (recordedMs < nextPreviewMs || recordedMs >= MaxRecordingMs) return;
                    nextPreviewMs = recordedMs + PreviewIntervalMs;
                    // One low-priority snapshot at a time. Each result replaces the previous preview.
                    try {
                        pending = std::async(std::launch::async,
                            [&stopPreview, snapshot = samples, rate, channels] {
                                try {
                                    return LocalText(ToWave(snapshot, rate, channels), stopPreview,
                                                     BELOW_NORMAL_PRIORITY_CLASS);
                                } catch (const VoiceFailure &) {
                                    return std::wstring{};
                                } catch (const std::exception &) {
                                    return std::wstring{};
                                }
                            });
                    } catch (const std::exception &) {
                        // Recording and the final pass can proceed if a preview cannot start.
                    }
                });
                state_ = State::Transcribing;
            } catch (const VoiceFailure &failure) {
                result_.error = failure.message;
            } catch (const std::exception &) {
                result_.error = L"麦克风录音失败";
            }
            stopPreview = true;
            if (pending.valid()) pending.wait();
            if (!wav.empty()) result_ = TranscribeVoice(wav, cancel_);
            state_.store(State::Done, std::memory_order_release);
        });
    } catch (const std::exception &) {
        state_ = State::Idle;
        error = L"无法启动录音线程";
        return false;
    }
    return true;
}
void VoiceInput::FinishRecording() { finish_ = true; }
void VoiceInput::Cancel() { cancel_ = true; }
void VoiceInput::Join() { if (worker_.joinable()) worker_.join(); }
VoiceInput::State VoiceInput::Status() const {
    const auto state = state_.load(std::memory_order_acquire);
    if (state == State::Idle || state == State::Done) return state;
    if (cancel_) return State::Canceling;
    return state == State::Recording && finish_ ? State::Transcribing : state;
}
std::optional<std::wstring> VoiceInput::TakePreview() {
    if (Status() != State::Recording) return {};
    std::lock_guard<std::mutex> lock(previewMutex_);
    auto result = std::move(preview_);
    preview_.reset();
    return result;
}
std::optional<VoiceResult> VoiceInput::TakeResult() {
    if (Status() != State::Done) return {};
    Join();
    preview_.reset();
    state_ = State::Idle;
    return cancel_ ? VoiceResult{} : std::move(result_);
}

} // namespace serialctl::ai
