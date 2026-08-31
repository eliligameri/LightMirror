#include "AudioEngine.h"

#include "AudioUtils.h"
#include "ProcessLoopback.h"
#include "StereoRingBuffer.h"

#include <algorithm>
#include <audioclient.h>
#include <avrt.h>
#include <cmath>
#include <cstring>
#include <mmdeviceapi.h>
#include <sstream>
#include <vector>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace lm {
namespace {

constexpr UINT32 kOutputRate = 48000;
constexpr float kOutputCeiling = 0.995f;

bool IsRetryableAudioError(HRESULT hr) {
    return hr == AUDCLNT_E_DEVICE_INVALIDATED ||
           hr == AUDCLNT_E_RESOURCES_INVALIDATED ||
           hr == AUDCLNT_E_SERVICE_NOT_RUNNING ||
           hr == AUDCLNT_E_ENDPOINT_CREATE_FAILED ||
           hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
           hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED);
}

size_t WaveFormatBytes(const WAVEFORMATEX* f) {
    return f ? sizeof(WAVEFORMATEX) + f->cbSize : 0;
}

std::vector<BYTE> CopyWaveFormat(const WAVEFORMATEX* f) {
    std::vector<BYTE> storage(WaveFormatBytes(f));
    if (!storage.empty()) std::memcpy(storage.data(), f, storage.size());
    return storage;
}

bool IsWritableStereo48k(const WAVEFORMATEX* f) {
    if (!f || f->nChannels != 2 || f->nSamplesPerSec != kOutputRate) return false;
    if (IsFloatFormat(f)) return f->wBitsPerSample == 32 && f->nBlockAlign == 8;
    if (IsPcmFormat(f)) {
        const UINT32 bytes = f->nBlockAlign / 2;
        return (f->wBitsPerSample == 16 && bytes == 2) ||
               (f->wBitsPerSample == 24 && bytes == 3) ||
               (f->wBitsPerSample == 32 && bytes == 4);
    }
    return false;
}

bool IsReadableFormat(const WAVEFORMATEX* f) {
    if (!f || f->nChannels == 0 || f->nChannels > 32 || f->nBlockAlign == 0) return false;
    if (IsFloatFormat(f)) return f->wBitsPerSample == 32 && f->nBlockAlign / f->nChannels == 4;
    if (IsPcmFormat(f)) {
        const UINT32 bytes = f->nBlockAlign / f->nChannels;
        return (f->wBitsPerSample == 16 && bytes == 2) ||
               (f->wBitsPerSample == 24 && bytes == 3) ||
               (f->wBitsPerSample == 32 && bytes == 4);
    }
    return false;
}

UINT32 RoundPeriod(UINT32 desired, UINT32 fundamental, UINT32 minPeriod, UINT32 maxPeriod) {
    if (fundamental == 0) fundamental = 1;
    desired = std::clamp(desired, minPeriod, maxPeriod);
    UINT32 rounded = ((desired + fundamental - 1) / fundamental) * fundamental;
    if (rounded > maxPeriod) {
        rounded = (maxPeriod / fundamental) * fundamental;
    }
    return std::clamp(rounded, minPeriod, maxPeriod);
}

UINT32 ChooseOutputPeriod(LatencyMode mode,
                          UINT32 defaultPeriod,
                          UINT32 fundamental,
                          UINT32 minPeriod,
                          UINT32 maxPeriod) {
    if (mode == LatencyMode::UltraLow) return minPeriod;
    if (mode == LatencyMode::Safe) return defaultPeriod;
    const UINT32 aboutThreeMs = (kOutputRate * 3) / 1000;
    const UINT32 desired = std::max(minPeriod, std::min(defaultPeriod, aboutThreeMs));
    return RoundPeriod(desired, fundamental, minPeriod, maxPeriod);
}

double QueueTargetMs(LatencyMode mode) {
    switch (mode) {
    case LatencyMode::UltraLow: return 5.0;
    case LatencyMode::Low: return 14.0;
    case LatencyMode::Safe: return 30.0;
    }
    return 14.0;
}

StereoFrame CubicInterpolate(const StereoFrame& y0,
                             const StereoFrame& y1,
                             const StereoFrame& y2,
                             const StereoFrame& y3,
                             double t) {
    auto interp = [t](float p0, float p1, float p2, float p3) {
        const double a0 = -0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3;
        const double a1 = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
        const double a2 = -0.5 * p0 + 0.5 * p2;
        const double a3 = p1;
        return static_cast<float>(((a0 * t + a1) * t + a2) * t + a3);
    };
    return {interp(y0.l, y1.l, y2.l, y3.l), interp(y0.r, y1.r, y2.r, y3.r)};
}

} // namespace

AudioEngine::AudioEngine() {
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

AudioEngine::~AudioEngine() {
    Stop();
    if (stopEvent_) CloseHandle(stopEvent_);
}

bool AudioEngine::Start(const std::wstring& sourceDeviceId,
                        const std::wstring& outputDeviceId,
                        CaptureMode captureMode,
                        LatencyMode latencyMode,
                        float volume) {
    if (outputDeviceId.empty() ||
        (!sourceDeviceId.empty() && sourceDeviceId == outputDeviceId) ||
        (captureMode == CaptureMode::EndpointLoopback && sourceDeviceId.empty())) {
        SetStatus(EngineState::Error,
                  !sourceDeviceId.empty() && sourceDeviceId == outputDeviceId
                      ? L"Source and output must be different devices."
                      : L"Select both a source and an output device.",
                  false);
        return false;
    }

    Stop();
    if (!stopEvent_) {
        SetStatus(EngineState::Error, L"Could not create the audio stop event.", false);
        return false;
    }

    ResetEvent(stopEvent_);
    volume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_release);
    underruns_.store(0, std::memory_order_release);
    overruns_.store(0, std::memory_order_release);
    estimatedLatencyMs_.store(0.0, std::memory_order_release);
    workerActive_.store(true, std::memory_order_release);
    SetStatus(EngineState::Starting, L"Starting audio mirror...", false);

    try {
        worker_ = std::thread(&AudioEngine::WorkerMain, this, sourceDeviceId, outputDeviceId,
                              captureMode, latencyMode);
    } catch (...) {
        workerActive_.store(false, std::memory_order_release);
        SetStatus(EngineState::Error, L"Could not create the audio thread.", false);
        return false;
    }
    return true;
}

void AudioEngine::Stop() {
    if (stopEvent_) SetEvent(stopEvent_);
    if (worker_.joinable()) worker_.join();
    workerActive_.store(false, std::memory_order_release);
    SetStatus(EngineState::Stopped, L"Stopped", false);
}

void AudioEngine::SetVolume(float volume) {
    volume_.store(std::clamp(volume, 0.0f, 1.0f), std::memory_order_release);
}

EngineSnapshot AudioEngine::Snapshot() const {
    EngineSnapshot s;
    s.state = state_.load(std::memory_order_acquire);
    s.estimatedBufferLatencyMs = estimatedLatencyMs_.load(std::memory_order_acquire);
    s.underruns = underruns_.load(std::memory_order_acquire);
    s.overruns = overruns_.load(std::memory_order_acquire);
    s.retryable = retryable_.load(std::memory_order_acquire);
    {
        std::scoped_lock lock(statusMutex_);
        s.status = status_;
    }
    return s;
}

void AudioEngine::SetStatus(EngineState state, std::wstring text, bool retryable) {
    {
        std::scoped_lock lock(statusMutex_);
        status_ = std::move(text);
    }
    retryable_.store(retryable, std::memory_order_release);
    state_.store(state, std::memory_order_release);
}

void AudioEngine::Fail(HRESULT hr, const wchar_t* context) {
    std::wstring text = context;
    text += L": ";
    text += HResultToString(hr);
    SetStatus(EngineState::Error, std::move(text), IsRetryableAudioError(hr));
    if (stopEvent_) SetEvent(stopEvent_);
}

void AudioEngine::WorkerMain(std::wstring sourceDeviceId,
                             std::wstring outputDeviceId,
                             CaptureMode captureMode,
                             LatencyMode latencyMode) {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr)) {
        Fail(coHr, L"COM initialization failed");
        workerActive_.store(false, std::memory_order_release);
        return;
    }

    DWORD mmcssTask = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcssTask);

    HANDLE captureEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HANDLE renderEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!captureEvent || !renderEvent) {
        Fail(HRESULT_FROM_WIN32(GetLastError()), L"Could not create WASAPI event handles");
        if (captureEvent) CloseHandle(captureEvent);
        if (renderEvent) CloseHandle(renderEvent);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        CoUninitialize();
        workerActive_.store(false, std::memory_order_release);
        return;
    }

    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> sourceDevice;
    ComPtr<IMMDevice> outputDevice;
    ComPtr<IAudioClient> captureAudioClient;
    ComPtr<IAudioCaptureClient> captureClient;
    ComPtr<IAudioClient> renderAudioClient;
    ComPtr<IAudioRenderClient> renderClient;
    WAVEFORMATEX* captureFormat = nullptr;
    bool captureFormatOwned = false;
    WAVEFORMATEXTENSIBLE cleanCaptureFormat = MakeStereo48kFloatFormat();
    bool cleanCaptureActive = false;
    HRESULT cleanCaptureFailure = S_OK;
    WAVEFORMATEX* outputMixFormat = nullptr;
    std::vector<BYTE> outputFormatStorage;
    WAVEFORMATEXTENSIBLE fallbackOutput = MakeStereo48kFloatFormat();
    const WAVEFORMATEX* outputFormat = nullptr;
    UINT32 renderBufferFrames = 0;
    UINT32 outputPeriodFrames = 0;
    REFERENCE_TIME captureDefaultPeriod = 0;

    auto cleanup = [&]() {
        if (renderAudioClient) renderAudioClient->Stop();
        if (captureAudioClient) captureAudioClient->Stop();
        if (captureFormatOwned && captureFormat) CoTaskMemFree(captureFormat);
        if (outputMixFormat) CoTaskMemFree(outputMixFormat);
        CloseHandle(captureEvent);
        CloseHandle(renderEvent);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        CoUninitialize();
        workerActive_.store(false, std::memory_order_release);
    };

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&enumerator));
    if (FAILED(hr)) { Fail(hr, L"Could not create the audio device enumerator"); cleanup(); return; }

    hr = enumerator->GetDevice(outputDeviceId.c_str(), &outputDevice);
    if (FAILED(hr)) { Fail(hr, L"Output device is unavailable"); cleanup(); return; }

    // Clean mode uses the endpoint-independent process-loopback virtual device. Excluding
    // this process captures the system/app mix but never recaptures our HDMI output.
    if (captureMode == CaptureMode::CleanSystem) {
        cleanCaptureFailure = ActivateCleanSystemLoopback(captureAudioClient, stopEvent_);
        if (SUCCEEDED(cleanCaptureFailure)) {
            cleanCaptureActive = true;
            captureFormat = &cleanCaptureFormat.Format;
        }
    }

    auto openEndpointCapture = [&]() -> HRESULT {
        if (sourceDeviceId.empty()) return E_INVALIDARG;
        hr = enumerator->GetDevice(sourceDeviceId.c_str(), &sourceDevice);
        if (FAILED(hr)) return hr;
        hr = sourceDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(captureAudioClient.ReleaseAndGetAddressOf()));
        if (FAILED(hr)) return hr;
        hr = captureAudioClient->GetMixFormat(&captureFormat);
        if (SUCCEEDED(hr)) captureFormatOwned = true;
        return hr;
    };

    // Preserve endpoint loopback both as an explicit mode and as automatic fallback.
    if (!cleanCaptureActive) {
        hr = openEndpointCapture();
        if (FAILED(hr)) { Fail(hr, L"Fallback source device is unavailable"); cleanup(); return; }
    }
    if (!IsReadableFormat(captureFormat)) {
        SetStatus(EngineState::Error,
                  L"Unsupported source mix format: " + FormatAudioFormat(captureFormat), false);
        cleanup(); return;
    }
    captureAudioClient->GetDevicePeriod(&captureDefaultPeriod, nullptr);
    hr = captureAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        AUDCLNT_STREAMFLAGS_LOOPBACK |
                                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                            (cleanCaptureActive
                                                 ? (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY)
                                                 : 0),
                                        0, 0, captureFormat, nullptr);
    if (FAILED(hr) && cleanCaptureActive) {
        cleanCaptureFailure = hr;
        cleanCaptureActive = false;
        captureAudioClient.Reset();
        captureFormat = nullptr;
        hr = openEndpointCapture();
        if (SUCCEEDED(hr) && IsReadableFormat(captureFormat)) {
            captureAudioClient->GetDevicePeriod(&captureDefaultPeriod, nullptr);
            hr = captureAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                                AUDCLNT_STREAMFLAGS_LOOPBACK |
                                                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                0, 0, captureFormat, nullptr);
        }
    }
    if (FAILED(hr)) { Fail(hr, L"Could not initialize clean capture or endpoint fallback"); cleanup(); return; }
    hr = captureAudioClient->SetEventHandle(captureEvent);
    if (FAILED(hr)) { Fail(hr, L"Could not attach source event handle"); cleanup(); return; }
    hr = captureAudioClient->GetService(IID_PPV_ARGS(&captureClient));
    if (FAILED(hr)) { Fail(hr, L"Could not get loopback capture service"); cleanup(); return; }

    // Render: prefer IAudioClient3 at the endpoint's 48 kHz stereo mix format. This permits
    // the smallest legal shared engine period without changing the endpoint's system format.
    bool usedAudioClient3 = false;
    ComPtr<IAudioClient3> renderClient3;
    hr = outputDevice->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr,
                                reinterpret_cast<void**>(renderClient3.GetAddressOf()));
    if (SUCCEEDED(hr) && renderClient3) {
        AudioClientProperties props{};
        props.cbSize = sizeof(props);
        props.eCategory = AudioCategory_Media;
        props.Options = AUDCLNT_STREAMOPTIONS_NONE;
        renderClient3->SetClientProperties(&props); // Best effort; failure is not fatal.

        hr = renderClient3->GetMixFormat(&outputMixFormat);
        if (SUCCEEDED(hr) && IsWritableStereo48k(outputMixFormat)) {
            outputFormatStorage = CopyWaveFormat(outputMixFormat);
            outputFormat = reinterpret_cast<const WAVEFORMATEX*>(outputFormatStorage.data());

            UINT32 defaultFrames = 0, fundamentalFrames = 0, minFrames = 0, maxFrames = 0;
            hr = renderClient3->GetSharedModeEnginePeriod(outputFormat, &defaultFrames,
                                                          &fundamentalFrames, &minFrames, &maxFrames);
            if (SUCCEEDED(hr)) {
                outputPeriodFrames = ChooseOutputPeriod(latencyMode, defaultFrames,
                                                        fundamentalFrames, minFrames, maxFrames);
                hr = renderClient3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                                                outputPeriodFrames,
                                                                outputFormat, nullptr);
                if (SUCCEEDED(hr)) {
                    hr = renderClient3.As(&renderAudioClient);
                    usedAudioClient3 = SUCCEEDED(hr);
                }
            }
        }
    }

    if (!usedAudioClient3) {
        renderClient3.Reset();
        renderAudioClient.Reset();
        hr = outputDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(renderAudioClient.GetAddressOf()));
        if (FAILED(hr)) { Fail(hr, L"Could not activate output WASAPI client"); cleanup(); return; }

        if (outputMixFormat && IsWritableStereo48k(outputMixFormat)) {
            outputFormatStorage = CopyWaveFormat(outputMixFormat);
            outputFormat = reinterpret_cast<const WAVEFORMATEX*>(outputFormatStorage.data());
        } else {
            outputFormat = &fallbackOutput.Format;
        }

        hr = renderAudioClient->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                                AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                            0, 0, outputFormat, nullptr);
        if (FAILED(hr)) { Fail(hr, L"Could not initialize 48 kHz stereo output"); cleanup(); return; }

        REFERENCE_TIME defaultPeriod = 0;
        if (SUCCEEDED(renderAudioClient->GetDevicePeriod(&defaultPeriod, nullptr))) {
            outputPeriodFrames = static_cast<UINT32>(std::max<REFERENCE_TIME>(1, defaultPeriod) *
                                                      kOutputRate / 10000000LL);
        }
    }

    if (!outputFormat || outputFormat->nChannels != 2 || outputFormat->nSamplesPerSec != kOutputRate) {
        SetStatus(EngineState::Error, L"Output could not be opened as 48 kHz stereo.", false);
        cleanup(); return;
    }

    hr = renderAudioClient->SetEventHandle(renderEvent);
    if (FAILED(hr)) { Fail(hr, L"Could not attach output event handle"); cleanup(); return; }
    hr = renderAudioClient->GetService(IID_PPV_ARGS(&renderClient));
    if (FAILED(hr)) { Fail(hr, L"Could not get output render service"); cleanup(); return; }
    hr = renderAudioClient->GetBufferSize(&renderBufferFrames);
    if (FAILED(hr)) { Fail(hr, L"Could not read output buffer size"); cleanup(); return; }

    const UINT32 sourceRate = captureFormat->nSamplesPerSec;
    const auto speakerOrder = BuildSpeakerOrder(captureFormat);
    StereoRingBuffer ring(std::max<size_t>(262144, static_cast<size_t>(sourceRate) * 2));
    float limiterGain = 1.0f;
    bool primed = false;
    bool everPrimed = false;
    bool havePrev = false;
    StereoFrame prev{};
    double phase = 0.0;
    double smoothedCorrection = 0.0;
    const double targetMs = QueueTargetMs(latencyMode);
    const size_t targetFrames = std::max<size_t>(4, static_cast<size_t>(std::ceil(sourceRate * targetMs / 1000.0)));
    double capturePeriodEstimateMs = captureDefaultPeriod > 0
        ? static_cast<double>(captureDefaultPeriod) / 10000.0
        : 10.0;
    LARGE_INTEGER qpcFrequency{};
    LARGE_INTEGER lastCaptureQpc{};
    QueryPerformanceFrequency(&qpcFrequency);

    std::wostringstream running;
    if (cleanCaptureActive) {
        running << L"Clean System Capture (endpoint-independent, G HUB device effects bypassed): ";
    } else if (captureMode == CaptureMode::CleanSystem) {
        running << L"Compatibility fallback to PRO X 2 Endpoint Loopback (clean capture unavailable: "
                << HResultToString(cleanCaptureFailure) << L"): ";
    } else {
        running << L"PRO X 2 Endpoint Loopback: ";
    }
    running << FormatAudioFormat(captureFormat)
            << L" -> 2ch / 48000 Hz"
            << (usedAudioClient3 ? L" (low-period WASAPI)" : L" (standard shared WASAPI)");
    SetStatus(EngineState::Running, running.str(), false);

    hr = captureAudioClient->Start();
    if (FAILED(hr)) { Fail(hr, L"Could not start loopback capture"); cleanup(); return; }
    hr = renderAudioClient->Start();
    if (FAILED(hr)) { Fail(hr, L"Could not start output rendering"); cleanup(); return; }

    HANDLE waits[3] = {stopEvent_, captureEvent, renderEvent};
    bool fatal = false;
    while (!fatal) {
        const DWORD wait = WaitForMultipleObjects(3, waits, FALSE, INFINITE);
        if (wait == WAIT_OBJECT_0) break;
        if (wait == WAIT_FAILED) {
            Fail(HRESULT_FROM_WIN32(GetLastError()), L"Audio wait failed");
            break;
        }

        if (wait == WAIT_OBJECT_0 + 1) {
            LARGE_INTEGER now{};
            if (qpcFrequency.QuadPart > 0 && QueryPerformanceCounter(&now)) {
                if (lastCaptureQpc.QuadPart != 0) {
                    const double deltaMs = static_cast<double>(now.QuadPart - lastCaptureQpc.QuadPart) *
                                           1000.0 / static_cast<double>(qpcFrequency.QuadPart);
                    if (deltaMs >= 0.1 && deltaMs <= 100.0) {
                        capturePeriodEstimateMs = capturePeriodEstimateMs * 0.9 + deltaMs * 0.1;
                    }
                }
                lastCaptureQpc = now;
            }

            // Drain every capture packet currently queued.
            while (true) {
                UINT32 packetFrames = 0;
                hr = captureClient->GetNextPacketSize(&packetFrames);
                if (FAILED(hr)) { Fail(hr, L"Loopback packet query failed"); fatal = true; break; }
                if (packetFrames == 0) break;

                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                hr = captureClient->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (FAILED(hr)) { Fail(hr, L"Loopback buffer read failed"); fatal = true; break; }

                if (frames > ring.Free()) {
                    const size_t toDrop = static_cast<size_t>(frames) - ring.Free();
                    ring.DropOldest(toDrop);
                    overruns_.fetch_add(1, std::memory_order_relaxed);
                    primed = false;
                    havePrev = false;
                    phase = 0.0;
                }

                const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                    // Keep newest data but reset interpolation history across discontinuities.
                    havePrev = false;
                    phase = 0.0;
                }

                for (UINT32 i = 0; i < frames; ++i) {
                    StereoFrame mixed{};
                    if (!silent && data) {
                        const BYTE* frame = data + static_cast<size_t>(i) * captureFormat->nBlockAlign;
                        mixed = DownmixFrame(frame, captureFormat, speakerOrder, limiterGain);
                    }
                    if (!ring.Push(mixed)) {
                        ring.DropOldest(1);
                        ring.Push(mixed);
                        overruns_.fetch_add(1, std::memory_order_relaxed);
                        primed = false;
                        havePrev = false;
                        phase = 0.0;
                    }
                }

                hr = captureClient->ReleaseBuffer(frames);
                if (FAILED(hr)) { Fail(hr, L"Loopback buffer release failed"); fatal = true; break; }
            }
        } else if (wait == WAIT_OBJECT_0 + 2) {
            UINT32 padding = 0;
            hr = renderAudioClient->GetCurrentPadding(&padding);
            if (FAILED(hr)) { Fail(hr, L"Output padding query failed"); break; }
            if (padding > renderBufferFrames) padding = renderBufferFrames;
            const UINT32 framesToWrite = renderBufferFrames - padding;
            if (framesToWrite == 0) continue;

            BYTE* outData = nullptr;
            hr = renderClient->GetBuffer(framesToWrite, &outData);
            if (FAILED(hr)) { Fail(hr, L"Output buffer acquisition failed"); break; }

            bool wroteAnyAudio = false;
            bool underflowThisPass = false;
            for (UINT32 i = 0; i < framesToWrite; ++i) {
                StereoFrame sample{};

                if (!primed && ring.Size() >= targetFrames && ring.Size() >= 3) {
                    primed = true;
                    everPrimed = true;
                    havePrev = false;
                    phase = 0.0;
                }

                if (primed && ring.Size() >= 3) {
                    const double fillError = static_cast<double>(ring.Size()) - static_cast<double>(targetFrames);
                    // Tiny adaptive ratio correction keeps independent endpoint clocks from slowly
                    // overflowing/underflowing the queue. Limited to +/-0.3% so pitch change is inaudible.
                    const double requestedCorrection =
                        std::clamp(fillError / (static_cast<double>(sourceRate) * 2.0),
                                   -0.003, 0.003);
                    // Roughly one-second smoothing prevents queue jitter from becoming
                    // short-term pitch/phase modulation while still tracking clock drift.
                    smoothedCorrection += (requestedCorrection - smoothedCorrection) * 0.000025;
                    const double ratio = (static_cast<double>(sourceRate) / kOutputRate) *
                                         (1.0 + smoothedCorrection);

                    const StereoFrame y1 = ring.Peek(0);
                    const StereoFrame y0 = havePrev ? prev : y1;
                    const StereoFrame y2 = ring.Peek(1);
                    const StereoFrame y3 = ring.Peek(2);
                    sample = CubicInterpolate(y0, y1, y2, y3, phase);

                    phase += ratio;
                    while (phase >= 1.0 && ring.Size() > 0) {
                        prev = ring.Peek(0);
                        havePrev = true;
                        ring.Pop(1);
                        phase -= 1.0;
                    }
                    wroteAnyAudio = true;
                } else if (primed) {
                    primed = false;
                    havePrev = false;
                    phase = 0.0;
                    underflowThisPass = true;
                }

                float gain = volume_.load(std::memory_order_relaxed);
                sample.l *= gain;
                sample.r *= gain;

                // Cubic interpolation can overshoot very slightly. Apply a linked final ceiling,
                // preserving stereo balance and avoiding hard integer/float overflow.
                const float peak = std::max(std::abs(sample.l), std::abs(sample.r));
                if (peak > kOutputCeiling) {
                    const float scale = kOutputCeiling / peak;
                    sample.l *= scale;
                    sample.r *= scale;
                }

                WriteStereoFrame(outData + static_cast<size_t>(i) * outputFormat->nBlockAlign,
                                 outputFormat, sample.l, sample.r);
            }

            if (underflowThisPass && everPrimed) {
                underruns_.fetch_add(1, std::memory_order_relaxed);
            }

            DWORD releaseFlags = 0;
            if (!wroteAnyAudio && ring.Size() == 0) {
                // The buffer is already written with zeros, but SILENT lets the engine optimize it.
                releaseFlags = AUDCLNT_BUFFERFLAGS_SILENT;
            }
            hr = renderClient->ReleaseBuffer(framesToWrite, releaseFlags);
            if (FAILED(hr)) { Fail(hr, L"Output buffer release failed"); break; }

            const double ringMs = static_cast<double>(ring.Size()) * 1000.0 / sourceRate;
            const double outputQueuedMs = static_cast<double>(padding + framesToWrite) * 1000.0 / kOutputRate;
            estimatedLatencyMs_.store(capturePeriodEstimateMs + ringMs + outputQueuedMs,
                                      std::memory_order_release);
        }
    }

    cleanup();
}

} // namespace lm
