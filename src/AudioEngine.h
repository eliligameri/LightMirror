#pragma once

#include "AudioTypes.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>

namespace lm {

class AudioEngine {
public:
    AudioEngine();
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    bool Start(const std::wstring& sourceDeviceId,
               const std::wstring& outputDeviceId,
               CaptureMode captureMode,
               LatencyMode latencyMode,
               float volume);
    void Stop();
    void SetVolume(float volume);

    EngineSnapshot Snapshot() const;
    bool IsWorkerActive() const { return workerActive_.load(std::memory_order_acquire); }

private:
    void WorkerMain(std::wstring sourceDeviceId,
                    std::wstring outputDeviceId,
                    CaptureMode captureMode,
                    LatencyMode latencyMode);
    void SetStatus(EngineState state, std::wstring text, bool retryable = false);
    void Fail(HRESULT hr, const wchar_t* context);

    std::atomic<float> volume_{1.0f};
    std::atomic<bool> workerActive_{false};
    std::atomic<double> estimatedLatencyMs_{0.0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> overruns_{0};
    std::atomic<bool> retryable_{false};
    std::atomic<EngineState> state_{EngineState::Stopped};

    mutable std::mutex statusMutex_;
    std::wstring status_ = L"Stopped";

    HANDLE stopEvent_ = nullptr;
    std::thread worker_;
};

} // namespace lm
