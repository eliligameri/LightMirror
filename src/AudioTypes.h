#pragma once

#include <cstdint>
#include <string>

namespace lm {

enum class LatencyMode : int {
    UltraLow = 0,
    Low = 1,
    Safe = 2,
};

enum class CaptureMode : int {
    CleanSystem = 0,
    EndpointLoopback = 1,
};

enum class EngineState : int {
    Stopped = 0,
    Starting,
    Running,
    Error,
};

struct DeviceInfo {
    std::wstring id;
    std::wstring name;
    bool active = true;
};

struct StereoFrame {
    float l = 0.0f;
    float r = 0.0f;
};

struct EngineSnapshot {
    EngineState state = EngineState::Stopped;
    std::wstring status;
    double estimatedBufferLatencyMs = 0.0;
    uint64_t underruns = 0;
    uint64_t overruns = 0;
    bool retryable = false;
};

} // namespace lm
