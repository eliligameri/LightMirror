#include "AudioEngine.h"
#include "DeviceManager.h"
#include "Settings.h"

#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <commctrl.h>
#include <cwctype>
#include <string>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"LightMirrorWindow";
constexpr UINT_PTR kUiTimer = 1;
constexpr UINT kUiTimerMs = 500;

enum ControlId : int {
    IDC_SOURCE = 101,
    IDC_OUTPUT,
    IDC_START_STOP,
    IDC_VOLUME,
    IDC_VOLUME_TEXT,
    IDC_LATENCY_MODE,
    IDC_LATENCY_TEXT,
    IDC_STATUS,
    IDC_CAPTURE_MODE,
};

bool ContainsInsensitive(std::wstring haystack, std::wstring needle) {
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::towlower);
    std::transform(needle.begin(), needle.end(), needle.begin(), ::towlower);
    return haystack.find(needle) != std::wstring::npos;
}

const wchar_t* StatePrefix(lm::EngineState state) {
    switch (state) {
    case lm::EngineState::Running: return L"Running - ";
    case lm::EngineState::Starting: return L"Starting - ";
    case lm::EngineState::Error: return L"Error - ";
    case lm::EngineState::Stopped: return L"Stopped - ";
    }
    return L"";
}

class MainWindow {
public:
    MainWindow()
        : settingsStore_(), settings_(settingsStore_.Load()) {}

    ~MainWindow() {
        engine_.Stop();
        if (font_) DeleteObject(font_);
    }

    bool Initialize(HWND hwnd) {
        hwnd_ = hwnd;
        NONCLIENTMETRICSW ncm{sizeof(ncm)};
        if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
            font_ = CreateFontIndirectW(&ncm.lfMessageFont);
        }

        CreateControls();
        RefreshDevices(true);

        SendMessageW(volume_, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
        SendMessageW(volume_, TBM_SETTICFREQ, 10, 0);
        SendMessageW(volume_, TBM_SETPOS, TRUE, settings_.volumePercent);
        engine_.SetVolume(settings_.volumePercent / 100.0f);

        SendMessageW(latencyMode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Ultra Low"));
        SendMessageW(latencyMode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Low"));
        SendMessageW(latencyMode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Safe"));
        SendMessageW(latencyMode_, CB_SETCURSEL, static_cast<int>(settings_.latencyMode), 0);

        SendMessageW(captureMode_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"Clean System Capture (recommended)"));
        SendMessageW(captureMode_, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(L"PRO X 2 Endpoint Loopback"));
        SendMessageW(captureMode_, CB_SETCURSEL, static_cast<int>(settings_.captureMode), 0);
        UpdateCaptureModeUi();

        UpdateVolumeText();
        UpdateUiFromEngine();
        SetTimer(hwnd_, kUiTimer, kUiTimerMs, nullptr);
        return true;
    }

    void OnSize(int width, int height) {
        const int margin = 18;
        const int labelW = 105;
        const int rowH = 28;
        const int editX = margin + labelW;
        const int editW = std::max(250, width - editX - margin);
        int y = 18;

        MoveWindow(GetDlgItem(hwnd_, 205), margin, y + 4, labelW - 8, 20, TRUE);
        MoveWindow(captureMode_, editX, y, editW, 200, TRUE);
        y += 42;

        MoveWindow(GetDlgItem(hwnd_, 201), margin, y + 4, labelW - 8, 20, TRUE);
        MoveWindow(source_, editX, y, editW, 300, TRUE);
        y += 42;

        MoveWindow(GetDlgItem(hwnd_, 202), margin, y + 4, labelW - 8, 20, TRUE);
        MoveWindow(output_, editX, y, editW, 300, TRUE);
        y += 42;

        MoveWindow(GetDlgItem(hwnd_, 203), margin, y + 4, labelW - 8, 20, TRUE);
        MoveWindow(volume_, editX, y, std::max(150, editW - 90), rowH, TRUE);
        MoveWindow(volumeText_, width - margin - 80, y + 4, 80, 20, TRUE);
        y += 42;

        MoveWindow(GetDlgItem(hwnd_, 204), margin, y + 4, labelW - 8, 20, TRUE);
        MoveWindow(latencyMode_, editX, y, 160, 200, TRUE);
        y += 42;

        MoveWindow(startStop_, margin, y, 150, 34, TRUE);
        MoveWindow(latencyText_, margin + 165, y + 7, std::max(180, width - margin * 2 - 165), 24, TRUE);
        y += 48;

        MoveWindow(status_, margin, y, width - margin * 2, std::max(40, height - y - margin), TRUE);
    }

    void OnCommand(WPARAM wParam, LPARAM lParam) {
        const int id = LOWORD(wParam);
        const int notification = HIWORD(wParam);

        if (id == IDC_START_STOP && notification == BN_CLICKED) {
            ToggleMirror();
            return;
        }

        if ((id == IDC_SOURCE || id == IDC_OUTPUT) && notification == CBN_SELCHANGE) {
            CaptureSelections();
            settingsStore_.Save(settings_);
            if (wantRunning_) RestartMirror();
            return;
        }

        if (id == IDC_CAPTURE_MODE && notification == CBN_SELCHANGE) {
            const int sel = static_cast<int>(SendMessageW(captureMode_, CB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel <= 1) settings_.captureMode = static_cast<lm::CaptureMode>(sel);
            UpdateCaptureModeUi();
            settingsStore_.Save(settings_);
            if (wantRunning_) RestartMirror();
            return;
        }

        if (id == IDC_LATENCY_MODE && notification == CBN_SELCHANGE) {
            const int sel = static_cast<int>(SendMessageW(latencyMode_, CB_GETCURSEL, 0, 0));
            if (sel >= 0 && sel <= 2) settings_.latencyMode = static_cast<lm::LatencyMode>(sel);
            settingsStore_.Save(settings_);
            if (wantRunning_) RestartMirror();
            return;
        }

        (void)lParam;
    }

    void OnHScroll(HWND control) {
        if (control != volume_) return;
        settings_.volumePercent = static_cast<int>(SendMessageW(volume_, TBM_GETPOS, 0, 0));
        engine_.SetVolume(settings_.volumePercent / 100.0f);
        settingsStore_.Save(settings_);
        UpdateVolumeText();
    }

    void OnTimer() {
        ++timerTicks_;
        if ((timerTicks_ % 4) == 0) {
            RefreshDevices(false);
            if (wantRunning_ && !engine_.IsWorkerActive()) {
                const auto snap = engine_.Snapshot();
                if (snap.state != lm::EngineState::Error || snap.retryable) {
                    TryStartMirror();
                }
            }
        }
        UpdateUiFromEngine();
    }

    void OnClose() {
        KillTimer(hwnd_, kUiTimer);
        CaptureSelections();
        settingsStore_.Save(settings_);
        engine_.Stop();
        DestroyWindow(hwnd_);
    }

private:
    void CreateControls() {
        auto make = [this](DWORD ex, const wchar_t* cls, const wchar_t* text, DWORD style,
                           int id) -> HWND {
            HWND h = CreateWindowExW(ex, cls, text, style | WS_CHILD | WS_VISIBLE,
                                     0, 0, 10, 10, hwnd_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                     GetModuleHandleW(nullptr), nullptr);
            if (h && font_) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            return h;
        };

        make(0, L"STATIC", L"Capture mode", SS_LEFT, 205);
        captureMode_ = make(WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"",
                            CBS_DROPDOWNLIST | WS_TABSTOP, IDC_CAPTURE_MODE);

        make(0, L"STATIC", L"Fallback source", SS_LEFT, 201);
        source_ = make(WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"",
                       CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_SOURCE);

        make(0, L"STATIC", L"Output", SS_LEFT, 202);
        output_ = make(WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"",
                       CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_OUTPUT);

        make(0, L"STATIC", L"Output volume", SS_LEFT, 203);
        volume_ = make(0, TRACKBAR_CLASSW, L"", TBS_AUTOTICKS | WS_TABSTOP, IDC_VOLUME);
        volumeText_ = make(0, L"STATIC", L"100%", SS_RIGHT, IDC_VOLUME_TEXT);

        make(0, L"STATIC", L"Latency mode", SS_LEFT, 204);
        latencyMode_ = make(WS_EX_CLIENTEDGE, WC_COMBOBOXW, L"",
                            CBS_DROPDOWNLIST | WS_TABSTOP, IDC_LATENCY_MODE);

        startStop_ = make(0, L"BUTTON", L"Start Mirror", BS_PUSHBUTTON | WS_TABSTOP, IDC_START_STOP);
        latencyText_ = make(0, L"STATIC", L"Estimated buffer latency: --", SS_LEFT, IDC_LATENCY_TEXT);
        status_ = make(WS_EX_CLIENTEDGE, L"STATIC", L"Stopped", SS_LEFT | SS_NOPREFIX, IDC_STATUS);
    }

    void RefreshDevices(bool firstRun) {
        const auto active = lm::DeviceManager::EnumerateActiveRenderDevices();

        if (firstRun) {
            if (settings_.sourceId.empty()) {
                for (const auto& d : active) {
                    if (ContainsInsensitive(d.name, L"PRO X 2")) {
                        settings_.sourceId = d.id;
                        settings_.sourceName = d.name;
                        break;
                    }
                }
                if (settings_.sourceId.empty()) settings_.sourceId = lm::DeviceManager::DefaultRenderDeviceId();
            }
            if (settings_.outputId.empty()) {
                for (const auto& d : active) {
                    if (ContainsInsensitive(d.name, L"HD60 X")) {
                        settings_.outputId = d.id;
                        settings_.outputName = d.name;
                        break;
                    }
                }
            }
        }

        FillDeviceCombo(source_, sourceItems_, active, settings_.sourceId, settings_.sourceName);
        FillDeviceCombo(output_, outputItems_, active, settings_.outputId, settings_.outputName);

        if (firstRun && settings_.outputId.empty()) {
            for (const auto& d : outputItems_) {
                if (d.active && d.id != settings_.sourceId) {
                    settings_.outputId = d.id;
                    settings_.outputName = d.name;
                    break;
                }
            }
            FillDeviceCombo(output_, outputItems_, active, settings_.outputId, settings_.outputName);
        }

        CaptureSelections();
        settingsStore_.Save(settings_);
    }

    static void FillDeviceCombo(HWND combo,
                                std::vector<lm::DeviceInfo>& items,
                                const std::vector<lm::DeviceInfo>& active,
                                const std::wstring& desiredId,
                                const std::wstring& desiredName) {
        SendMessageW(combo, WM_SETREDRAW, FALSE, 0);
        SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        items = active;
        int selected = -1;
        for (size_t i = 0; i < items.size(); ++i) {
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(items[i].name.c_str()));
            if (!desiredId.empty() && items[i].id == desiredId) selected = static_cast<int>(i);
        }

        if (!desiredId.empty() && selected < 0) {
            lm::DeviceInfo missing{desiredId,
                                   desiredName.empty() ? L"Saved audio device" : desiredName,
                                   false};
            items.push_back(missing);
            const std::wstring display = L"[Disconnected] " + missing.name;
            SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
            selected = static_cast<int>(items.size() - 1);
        }

        if (selected < 0 && !items.empty()) selected = 0;
        SendMessageW(combo, CB_SETCURSEL, selected, 0);
        SendMessageW(combo, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(combo, nullptr, TRUE);
    }

    void CaptureSelections() {
        auto captureOne = [](HWND combo, const std::vector<lm::DeviceInfo>& items,
                             std::wstring& id, std::wstring& name) {
            const int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
            if (sel >= 0 && static_cast<size_t>(sel) < items.size()) {
                id = items[sel].id;
                name = items[sel].name;
            }
        };
        captureOne(source_, sourceItems_, settings_.sourceId, settings_.sourceName);
        captureOne(output_, outputItems_, settings_.outputId, settings_.outputName);
    }

    bool SelectedDevicesActive() const {
        auto isActive = [](HWND combo, const std::vector<lm::DeviceInfo>& items) {
            const int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
            return sel >= 0 && static_cast<size_t>(sel) < items.size() && items[sel].active;
        };
        return isActive(output_, outputItems_) &&
               (settings_.captureMode == lm::CaptureMode::CleanSystem ||
                isActive(source_, sourceItems_));
    }

    void ToggleMirror() {
        if (wantRunning_) {
            wantRunning_ = false;
            engine_.Stop();
            SetWindowTextW(startStop_, L"Start Mirror");
        } else {
            wantRunning_ = true;
            SetWindowTextW(startStop_, L"Stop Mirror");
            CaptureSelections();
            settingsStore_.Save(settings_);
            TryStartMirror();
        }
        UpdateUiFromEngine();
    }

    void RestartMirror() {
        engine_.Stop();
        TryStartMirror();
    }

    void TryStartMirror() {
        if (!wantRunning_ || engine_.IsWorkerActive()) return;
        CaptureSelections();
        if (!SelectedDevicesActive()) {
            SetWindowTextW(status_, L"Waiting for the selected audio device(s) to reconnect...");
            return;
        }
        engine_.Start(settings_.sourceId, settings_.outputId, settings_.captureMode,
                      settings_.latencyMode,
                      settings_.volumePercent / 100.0f);
    }

    void UpdateCaptureModeUi() {
        const bool endpoint = settings_.captureMode == lm::CaptureMode::EndpointLoopback;
        EnableWindow(source_, endpoint ? TRUE : FALSE);
        SetWindowTextW(GetDlgItem(hwnd_, 201), endpoint ? L"Source" : L"Fallback source");
    }

    void UpdateVolumeText() {
        const std::wstring text = std::to_wstring(settings_.volumePercent) + L"%";
        SetWindowTextW(volumeText_, text.c_str());
    }

    void UpdateUiFromEngine() {
        const auto snap = engine_.Snapshot();
        std::wstring statusText = StatePrefix(snap.state);
        statusText += snap.status;
        if (snap.underruns || snap.overruns) {
            statusText += L"\r\nXRuns: ";
            statusText += std::to_wstring(snap.underruns);
            statusText += L" underrun(s), ";
            statusText += std::to_wstring(snap.overruns);
            statusText += L" overrun(s)";
        }
        SetWindowTextW(status_, statusText.c_str());

        wchar_t latency[128]{};
        if (snap.state == lm::EngineState::Running && snap.estimatedBufferLatencyMs > 0.0) {
            swprintf_s(latency, L"Estimated buffer latency: %.1f ms", snap.estimatedBufferLatencyMs);
        } else {
            wcscpy_s(latency, L"Estimated buffer latency: --");
        }
        SetWindowTextW(latencyText_, latency);
        SetWindowTextW(startStop_, wantRunning_ ? L"Stop Mirror" : L"Start Mirror");
    }

    HWND hwnd_ = nullptr;
    HWND source_ = nullptr;
    HWND captureMode_ = nullptr;
    HWND output_ = nullptr;
    HWND startStop_ = nullptr;
    HWND volume_ = nullptr;
    HWND volumeText_ = nullptr;
    HWND latencyMode_ = nullptr;
    HWND latencyText_ = nullptr;
    HWND status_ = nullptr;
    HFONT font_ = nullptr;

    lm::AudioEngine engine_;
    lm::SettingsStore settingsStore_;
    lm::AppSettings settings_;
    std::vector<lm::DeviceInfo> sourceItems_;
    std::vector<lm::DeviceInfo> outputItems_;
    bool wantRunning_ = false;
    unsigned timerTicks_ = 0;
};

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    MainWindow* window = reinterpret_cast<MainWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        window = reinterpret_cast<MainWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    }

    switch (msg) {
    case WM_CREATE:
        return window && window->Initialize(hwnd) ? 0 : -1;
    case WM_SIZE:
        if (window) window->OnSize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_COMMAND:
        if (window) window->OnCommand(wParam, lParam);
        return 0;
    case WM_HSCROLL:
        if (window) window->OnHScroll(reinterpret_cast<HWND>(lParam));
        return 0;
    case WM_TIMER:
        if (window && wParam == kUiTimer) window->OnTimer();
        return 0;
    case WM_CLOSE:
        if (window) window->OnClose();
        else DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(coHr)) {
        MessageBoxW(nullptr, L"LightMirror could not initialize COM.", L"LightMirror", MB_ICONERROR);
        return 1;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        CoUninitialize();
        return 1;
    }

    auto* window = new MainWindow();
    HWND hwnd = CreateWindowExW(0, kWindowClass, L"LightMirror",
                                WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                                CW_USEDEFAULT, CW_USEDEFAULT, 680, 410,
                                nullptr, nullptr, instance, window);
    if (!hwnd) {
        delete window;
        CoUninitialize();
        return 1;
    }

    RECT rc{};
    GetClientRect(hwnd, &rc);
    window->OnSize(rc.right - rc.left, rc.bottom - rc.top);

    ShowWindow(hwnd, showCommand);
    UpdateWindow(hwnd);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    delete window;
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}
