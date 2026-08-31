#pragma once

#include "AudioTypes.h"

#include <string>

namespace lm {

struct AppSettings {
    std::wstring sourceId;
    std::wstring sourceName;
    std::wstring outputId;
    std::wstring outputName;
    int volumePercent = 100;
    LatencyMode latencyMode = LatencyMode::UltraLow;
    CaptureMode captureMode = CaptureMode::CleanSystem;
};

class SettingsStore {
public:
    SettingsStore();
    AppSettings Load() const;
    void Save(const AppSettings& settings) const;
    const std::wstring& Path() const { return path_; }

private:
    std::wstring path_;
};

} // namespace lm
