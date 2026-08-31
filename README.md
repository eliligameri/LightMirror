# LightMirror

LightMirror is a tiny native Windows x64 application for one job: mirror a Windows render endpoint with WASAPI loopback, downmix multichannel audio to stereo, and send the stereo copy to a second render endpoint at 48 kHz.

It is designed for a dual-PC streaming path such as:

```text
Games / Discord / Windows
        |
        v
PRO X 2 LIGHTSPEED render endpoint (7.1 configured by Logitech G HUB)
        |-------------------------------> headphones (unchanged)
        |
        +-- WASAPI loopback --> LightMirror 7.1 -> 2.0 --> HD60 X HDMI audio --> streaming Mac
```

LightMirror **does not change the Windows default playback device**, does not install a driver, does not create a virtual cable, and does not modify Logitech G HUB or its surround configuration.

## What it does

- Captures the selected **render endpoint** with **WASAPI loopback**.
- Uses **event-driven shared WASAPI** for loopback capture.
- Reads the source endpoint's actual Windows mix format and channel mask.
- Correctly understands Windows speaker-mask ordering, including 7.1 home theater (`FL, FR, FC, LFE, BL, BR, SL, SR`) and 7.1-wide layouts.
- Downmixes to stereo with these coefficients:
  - Front Left / Front Right: `1.0` to their respective side.
  - Center: `-3 dB` to both left and right.
  - Side / rear channels: `-3 dB` to their corresponding side.
  - LFE: `-6 dB` to both sides.
  - Back-center / top-center channels: split conservatively between left and right.
- Uses a linked stereo peak limiter with immediate attack and slow release so summed multichannel peaks do not clip.
- Sends **48 kHz, 2-channel** audio to the selected output endpoint.
- Prefers the output endpoint's native 48 kHz stereo shared format and `IAudioClient3` minimum-period rendering when supported.
- Falls back to standard event-driven shared WASAPI with Windows' built-in shared-mode conversion when the endpoint cannot use the `IAudioClient3` path.
- Keeps a small adaptive queue and a tiny drift correction so the independent source/output device clocks do not slowly overrun or underrun each other.
- Runs audio capture, downmix, resampling, and rendering on one MMCSS **Pro Audio** worker thread.
- Remembers source, output, volume, and latency mode in `%LOCALAPPDATA%\LightMirror\settings.ini`.
- If a selected endpoint disappears, LightMirror stops cleanly, keeps the saved device ID, and automatically retries after it reconnects.

## Latency modes

The modes trade queue safety against buffering. Exact latency is determined by Windows' audio-engine periods, the Logitech/Elgato drivers, and HDMI/capture hardware.

| Mode | Render period | Adaptive queue target | Intended use |
| --- | --- | --- | --- |
| Ultra Low | Minimum period reported by `IAudioClient3` | ~5 ms | Lowest practical software buffering; least tolerant of scheduling/driver jitter |
| Low | About 3 ms where legal, otherwise nearest supported period | ~14 ms | Recommended starting point |
| Safe | Default shared engine period | ~30 ms | More protection from underruns |

The UI's **Estimated buffer latency** is a software-side estimate based on the source engine period, LightMirror queue, and current output padding. It is **not** a guarantee and does not include HDMI transport, HD60 X capture latency, the streaming Mac, or monitoring software.

### Important WASAPI limitation

Loopback streams must use shared-mode `IAudioClient::Initialize`; `IAudioClient3::InitializeSharedAudioStream` does not support the loopback flag. Windows 10 version 1703 and newer supports event signaling directly for loopback capture, which LightMirror uses. The low-period `IAudioClient3` optimization is therefore applied on the **HD60 X render side**, not the loopback side.

Microsoft references:

- https://learn.microsoft.com/windows/win32/coreaudio/loopback-recording
- https://learn.microsoft.com/windows/win32/api/audioclient/nf-audioclient-iaudioclient-initialize
- https://learn.microsoft.com/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-getsharedmodeengineperiod
- https://learn.microsoft.com/windows/win32/api/audioclient/nf-audioclient-iaudioclient3-initializesharedaudiostream
- https://learn.microsoft.com/windows-hardware/drivers/audio/channel-mask

## Requirements

- Windows 10 1703 or newer; Windows 11 is recommended.
- Visual Studio 2022 Build Tools or Visual Studio 2022 with the **Desktop development with C++** workload.
- CMake 3.24+.
- No third-party runtime libraries.

Release builds use the static MSVC runtime (`/MT`), so the resulting `LightMirror.exe` is portable in the normal Windows sense: copy the executable to another compatible Windows x64 machine and run it without installing LightMirror-specific dependencies.

## Build locally

From a **Developer Command Prompt for VS 2022**:

```bat
build.bat
```

Or manually:

```bat
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
```

The executable is produced at:

```text
build\Release\LightMirror.exe
```

## GitHub Actions

`.github/workflows/build.yml` builds an x64 Release executable on `windows-latest`:

- on every push;
- manually through **Actions -> Build LightMirror -> Run workflow**.

The workflow also runs a small downmix/ring-buffer test executable with CTest, then uploads an artifact named **LightMirror-windows-x64** containing `LightMirror.exe` and this README.

## Recommended setup for the requested PRO X 2 + HD60 X path

1. Leave **PRO X 2 LIGHTSPEED** as your normal/default Windows playback device.
2. Leave Logitech G HUB surround enabled exactly as you normally use it.
3. In LightMirror, select the **PRO X 2 LIGHTSPEED render endpoint** as **Source**.
4. Select the **Elgato HD60 X HDMI audio render endpoint** as **Output**.
5. Start with **Low** mode. If the XRuns counter stays at zero during gaming, try **Ultra Low**.
6. Keep output volume at 100% unless the HDMI feed needs attenuation.

LightMirror never calls `SetDefaultAudioEndpoint`, never changes a speaker configuration, and never writes G HUB settings.

## Verifying that the source is really 7.1

When mirroring starts, the status line shows the source mix format. For the desired discrete 7.1 downmix path, it should report something like:

```text
8ch / 48000 Hz / 32-bit float -> 2ch / 48000 Hz
```

If it reports **2ch**, Windows/G HUB is exposing a stereo mix format at the loopback point, so LightMirror can only mirror/downmix the channels Windows actually presents there. LightMirror deliberately does not change that configuration for you.

Also note that third-party audio APO/driver processing can be placed at different points in the Windows render graph. WASAPI loopback captures the render endpoint's system mix, but an application cannot force the loopback tap to sit after every vendor-specific hardware/driver DSP stage. The app therefore leaves G HUB completely untouched and reports the format it actually receives.

## Downmix and clipping behavior

The downmix follows the `WAVEFORMATEXTENSIBLE.dwChannelMask` channel order instead of assuming a fixed array ordering. This matters because Windows defines 7.1 home theater as mask `0x63F` (FL, FR, FC, LFE, BL, BR, SL, SR), while 7.1-wide uses a different mask/order.

Multiple full-scale channels can mathematically sum above full scale. Rather than permanently attenuating everything by ~10 dB, LightMirror keeps normal content at useful level and applies a **linked stereo limiter** only when the sum would exceed the output ceiling. That preserves left/right balance and prevents integer or float overflow. A second linked ceiling catches the very small overshoot that cubic interpolation can introduce.

## Sample-rate handling and clock drift

The HD60 X side is always submitted to WASAPI as **48 kHz stereo**. If its shared mix format is already a writable 48 kHz stereo PCM/float format, LightMirror writes that format directly. Otherwise it submits 48 kHz stereo float and enables Windows' built-in shared-mode conversion.

The source is normally expected to be 48 kHz for this use case. If the source endpoint runs at a different rate, LightMirror uses lightweight cubic interpolation while converting to the 48 kHz output timeline. The same resampler applies a very small, bounded (`+/-0.3%`) adaptive ratio correction to absorb long-term clock drift between the two physical endpoints. For the common 48 kHz-to-48 kHz path, this is effectively an almost-unity interpolation ratio with minimal CPU cost.

## Troubleshooting

**The source dropdown does not show PRO X 2**  
Make sure the headset render endpoint is enabled in Windows and G HUB. LightMirror lists active Windows render endpoints.

**The output dropdown does not show HD60 X**  
Make sure the HD60 X is connected and its HDMI audio render endpoint is enabled.

**Status shows 2ch source**  
LightMirror is receiving stereo from the Windows endpoint. Verify the speaker/surround configuration externally; LightMirror intentionally does not alter it.

**XRuns increase in Ultra Low**  
Switch to **Low**. If they still increase under game/encoder load, use **Safe**. Ultra Low intentionally sacrifices jitter margin.

**The device was unplugged**  
Keep LightMirror in the started state. The UI will show the saved endpoint as disconnected and retry when the same Windows endpoint ID returns.

**Source and output are the same device**  
LightMirror rejects this configuration because loopback would capture LightMirror's own render stream and create a feedback loop.

## Repository layout

```text
LightMirror/
  .github/workflows/build.yml
  CMakeLists.txt
  LightMirror.manifest
  LightMirror.rc
  build.bat
  README.md
  src/
    AudioEngine.cpp
    AudioEngine.h
    AudioTypes.h
    AudioUtils.cpp
    AudioUtils.h
    DeviceManager.cpp
    DeviceManager.h
    Settings.cpp
    Settings.h
    StereoRingBuffer.h
    main.cpp
  tests/
    AudioTests.cpp
```

## License

No license file is included. Add the license you want before publishing the repository publicly.
