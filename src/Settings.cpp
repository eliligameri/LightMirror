#include "Settings.h"

#include <algorithm>
#include <filesystem>
#include <windows.h>

namespace lm {
namespace {

std::wstring ReadIniString(const std::wstring& path, const wchar_t* key) {
    std::wstring buffer(32768, L'\0');
    const DWORD len = GetPrivateProfileStringW(L"LightMirror", key, L"", buffer.data(),
                                               static_cast<DWORD>(buffer.size()), path.c_str());
    buffer.resize(len);
    return buffer;
}

int ReadIniInt(const std::wstring& path, const wchar_t* key, int fallback) {
    return GetPrivateProfileIntW(L"LightMirror", key, fallback, path.c_str());
}

void WriteIni(const std::wstring& path, const wchar_t* key, const std::wstring& value) {
    WritePrivateProfileStringW(L"LightMirror", key, value.c_str(), path.c_str());
}

void WriteIniInt(const std::wstring& path, const wchar_t* key, int value) {
    WriteIni(path, key, std::to_wstring(value));
}

} // namespace

SettingsStore::SettingsStore() {
    wchar_t localAppData[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    std::filesystem::path dir;
    if (len > 0 && len < MAX_PATH) {
        dir = std::filesystem::path(localAppData) / L"LightMirror";
    } else {
        wchar_t exePath[MAX_PATH]{};
        GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        dir = std::filesystem::path(exePath).parent_path();
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    path_ = (dir / L"settings.ini").wstring();
}

AppSettings SettingsStore::Load() const {
    AppSettings s;
    s.sourceId = ReadIniString(path_, L"SourceId");
    s.sourceName = ReadIniString(path_, L"SourceName");
    s.outputId = ReadIniString(path_, L"OutputId");
    s.outputName = ReadIniString(path_, L"OutputName");
    s.volumePercent = std::clamp(ReadIniInt(path_, L"Volume", 100), 0, 100);
    const int mode = std::clamp(ReadIniInt(path_, L"LatencyMode", 0), 0, 2);
    s.latencyMode = static_cast<LatencyMode>(mode);
    const int captureMode = std::clamp(ReadIniInt(path_, L"CaptureMode", 0), 0, 1);
    s.captureMode = static_cast<CaptureMode>(captureMode);
    return s;
}

void SettingsStore::Save(const AppSettings& s) const {
    WriteIni(path_, L"SourceId", s.sourceId);
    WriteIni(path_, L"SourceName", s.sourceName);
    WriteIni(path_, L"OutputId", s.outputId);
    WriteIni(path_, L"OutputName", s.outputName);
    WriteIniInt(path_, L"Volume", s.volumePercent);
    WriteIniInt(path_, L"LatencyMode", static_cast<int>(s.latencyMode));
    WriteIniInt(path_, L"CaptureMode", static_cast<int>(s.captureMode));
}

} // namespace lm
