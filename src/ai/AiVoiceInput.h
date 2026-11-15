#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace serialctl::ai {

inline constexpr wchar_t VoiceWorkerArgument[] = L"--serialctl-voice-worker";
int RunVoiceWorker(const wchar_t *input, const wchar_t *output);

struct VoiceResult {
    std::wstring text, error;
};

// Transcribe a WAV locally for insertion into the editable draft.
VoiceResult TranscribeVoice(const std::vector<std::uint8_t> &wav, const std::atomic_bool &cancel);

class VoiceInput {
  public:
    enum class State { Idle, Recording, Transcribing, Canceling, Done };
    ~VoiceInput();
    bool Start(std::wstring &error);
    void FinishRecording();
    void Cancel();
    void Join();
    State Status() const;
    std::optional<std::wstring> TakePreview();
    std::optional<VoiceResult> TakeResult();

  private:
    friend struct VoiceInputTestAccess;
    using SamplesReady = std::function<void(const std::vector<std::uint8_t> &, unsigned rate, unsigned channels)>;
    using Capture = std::vector<std::uint8_t> (*)(const std::atomic_bool &finish, const std::atomic_bool &cancel,
                                                  const SamplesReady &);
    bool StartCapture(std::wstring &error, Capture);

    std::atomic<State> state_{State::Idle};
    std::atomic_bool finish_{false}, cancel_{false};
    std::thread worker_;
    std::mutex previewMutex_;
    std::optional<std::wstring> preview_;
    VoiceResult result_;
};

} // namespace serialctl::ai
