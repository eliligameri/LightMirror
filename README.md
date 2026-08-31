# LightMirror

LightMirror is a tiny native Windows x64 audio mirror for dual-PC streaming. It sends a low-latency 48 kHz stereo PCM copy of Windows audio to an output such as the Elgato HD60 X without changing the normal playback device, Logitech G HUB settings, or installing a virtual audio driver.

## Capture modes

### Clean System Capture (recommended)

Uses Windows endpoint-independent process loopback to capture all application audio except LightMirror's own process tree. This prevents output feedback and avoids the Logitech render endpoint as the capture tap:

```text
Games / Discord / Windows audio
        +--> normal path --> PRO X 2 --> G HUB 7.1 --> headphones
        +--> clean process loopback --> LightMirror --> 48 kHz stereo --> HD60 X
```

Microsoft documents process loopback as requiring Windows build 20348 or newer. LightMirror attempts it at runtime and falls back cleanly on older/unsupported builds. Protected audio and some apps with unusual routing may not be available.

If clean activation or initialization is unavailable, LightMirror automatically opens the saved fallback endpoint and clearly reports **Compatibility fallback to PRO X 2 Endpoint Loopback**.

### PRO X 2 Endpoint Loopback

Preserves the original WASAPI endpoint-loopback behavior. Because it taps the Logitech render endpoint, vendor APO/G HUB processing may already be present. The capture mode is remembered in `%LOCALAPPDATA%\LightMirror\settings.ini`.

## Audio quality and latency

- Clean capture requests 48 kHz, 32-bit float stereo and sends 48 kHz stereo PCM/float. There is no lossy encoding.
- Stereo clean capture passes through at unity gain in float space.
- Multichannel fallback uses fixed headroom: front `0.45`, center `0.25` per side, side/rear `0.125`, and LFE only `0.04` per side. A correlated 7.1 frame stays below full scale.
- The linked peak guard is an emergency clipping safety net, not routine compression.
- Event-driven shared WASAPI is used on both sides; `IAudioClient3` selects a small output period when supported.
- One MMCSS **Pro Audio** worker owns capture, conversion, buffering, and rendering. Stop/join and asynchronous activation completion are synchronized before release.
- Lightweight cubic resampling and a smoothed, bounded correction keep independent device clocks aligned.

| Mode | Queue target | Use |
| --- | ---: | --- |
| Ultra Low | ~5 ms | Lowest buffering; least jitter tolerance |
| Low | ~14 ms | Recommended |
| Safe | ~30 ms | Most underrun protection |

## Recommended setup

1. Keep PRO X 2 LIGHTSPEED as normal Windows playback and leave G HUB configured normally.
2. Select **Clean System Capture (recommended)**.
3. Keep PRO X 2 selected as **Fallback source**.
4. Select the Elgato HD60 X audio render endpoint as **Output**.
5. Start with **Low** latency and 100% volume.
6. Confirm status begins with **Clean System Capture**. A compatibility-fallback message means endpoint capture is active.

LightMirror never changes a default endpoint, speaker layout, or G HUB setting.

## Requirements and build

- Windows x64. Clean mode requires build 20348 or newer; endpoint fallback works on Windows 10 1703+.
- Visual Studio 2022 Build Tools with **Desktop development with C++**.
- CMake 3.24+ and a Windows SDK containing `audioclientactivationparams.h`.

From a Developer Command Prompt for VS 2022:

```bat
build.bat
```

Or:

```bat
cmake -S . -B build -A x64 -DLIGHTMIRROR_BUILD_TESTS=ON
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

The statically linked executable is produced at `build\Release\LightMirror.exe`.

## GitHub Actions

`.github/workflows/build.yml` performs an x64 Release build on push or manual dispatch, runs tests, and uploads `LightMirror.exe`, this README, and `SHA256SUMS.txt` as **LightMirror-windows-x64**.

## Troubleshooting

**Clean mode falls back**

The Windows API or clean stream initialization was unavailable. The status includes the error; keep a valid fallback endpoint selected.

**An app is silent in clean mode**

Protected content and apps with unusual capture policy/routing may not participate. Use endpoint loopback for that case.

**XRuns increase**

Move from Ultra Low to Low or Safe.

## Microsoft references

- [Application loopback sample](https://github.com/microsoft/Windows-classic-samples/tree/main/Samples/ApplicationLoopback)
- [WASAPI loopback recording](https://learn.microsoft.com/windows/win32/coreaudio/loopback-recording)
- [ActivateAudioInterfaceAsync](https://learn.microsoft.com/windows/win32/api/mmdeviceapi/nf-mmdeviceapi-activateaudiointerfaceasync)
- [Windows channel masks](https://learn.microsoft.com/windows-hardware/drivers/audio/channel-mask)

## License

No license file is included. Add the intended license before publishing publicly.
