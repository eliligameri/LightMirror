#include "AudioUtils.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <windows.h>

namespace lm {
namespace {

// Fixed-headroom broadcast-style matrix. A fully correlated 7.1 frame sums to
// 0.99 per side, so ordinary content never drives the emergency peak guard.
constexpr float kFront = 0.45f;
constexpr float kCenter = 0.25f;
constexpr float kSurround = 0.125f;
constexpr float kLfe = 0.04f; // LFE is effects-only, not a second bass copy.
constexpr float kShared = 0.08f;
constexpr float kLimiterCeiling = 0.995f;

GUID EffectiveSubFormat(const WAVEFORMATEX* format) {
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return ext->SubFormat;
    }
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        return KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    if (format->wFormatTag == WAVE_FORMAT_PCM) {
        return KSDATAFORMAT_SUBTYPE_PCM;
    }
    return GUID_NULL;
}

float ClampUnit(float v) {
    return std::clamp(v, -1.0f, 1.0f);
}

void AddSpeakerContribution(DWORD speaker, float sample, StereoFrame& out) {
    switch (speaker) {
    case SPEAKER_FRONT_LEFT:
        out.l += sample * kFront;
        break;
    case SPEAKER_FRONT_RIGHT:
        out.r += sample * kFront;
        break;
    case SPEAKER_FRONT_CENTER:
        out.l += sample * kCenter;
        out.r += sample * kCenter;
        break;
    case SPEAKER_LOW_FREQUENCY:
        out.l += sample * kLfe;
        out.r += sample * kLfe;
        break;
    case SPEAKER_BACK_LEFT:
    case SPEAKER_SIDE_LEFT:
    case SPEAKER_FRONT_LEFT_OF_CENTER:
    case SPEAKER_TOP_FRONT_LEFT:
    case SPEAKER_TOP_BACK_LEFT:
        out.l += sample * kSurround;
        break;
    case SPEAKER_BACK_RIGHT:
    case SPEAKER_SIDE_RIGHT:
    case SPEAKER_FRONT_RIGHT_OF_CENTER:
    case SPEAKER_TOP_FRONT_RIGHT:
    case SPEAKER_TOP_BACK_RIGHT:
        out.r += sample * kSurround;
        break;
    case SPEAKER_BACK_CENTER:
    case SPEAKER_TOP_CENTER:
    case SPEAKER_TOP_FRONT_CENTER:
    case SPEAKER_TOP_BACK_CENTER:
        out.l += sample * kShared;
        out.r += sample * kShared;
        break;
    default:
        // Unknown channels are shared conservatively to preserve content.
        out.l += sample * kShared;
        out.r += sample * kShared;
        break;
    }
}

} // namespace

std::wstring HResultToString(HRESULT hr) {
    wchar_t* raw = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD len = FormatMessageW(flags, nullptr, static_cast<DWORD>(hr),
                                     MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                     reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    std::wostringstream oss;
    oss << L"0x" << std::hex << std::uppercase << static_cast<unsigned long>(hr);
    if (len && raw) {
        std::wstring msg(raw, len);
        LocalFree(raw);
        while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n' || msg.back() == L' ')) {
            msg.pop_back();
        }
        if (!msg.empty()) {
            oss << L" (" << msg << L")";
        }
    }
    return oss.str();
}

std::wstring FormatAudioFormat(const WAVEFORMATEX* format) {
    if (!format) return L"unknown";
    std::wostringstream oss;
    oss << format->nChannels << L"ch / " << format->nSamplesPerSec << L" Hz / "
        << format->wBitsPerSample << L"-bit";
    if (IsFloatFormat(format)) oss << L" float";
    else if (IsPcmFormat(format)) oss << L" PCM";
    return oss.str();
}

bool IsFloatFormat(const WAVEFORMATEX* format) {
    if (!format) return false;
    const GUID sub = EffectiveSubFormat(format);
    return IsEqualGUID(sub, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) != FALSE;
}

bool IsPcmFormat(const WAVEFORMATEX* format) {
    if (!format) return false;
    const GUID sub = EffectiveSubFormat(format);
    return IsEqualGUID(sub, KSDATAFORMAT_SUBTYPE_PCM) != FALSE;
}

DWORD ChannelMaskForFormat(const WAVEFORMATEX* format) {
    if (!format) return 0;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format)->dwChannelMask;
    }

    switch (format->nChannels) {
    case 1: return SPEAKER_FRONT_CENTER;
    case 2: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    case 4: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    case 6: return SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                   SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    case 8: return KSAUDIO_SPEAKER_7POINT1_SURROUND;
    default: return 0;
    }
}

float ReadSampleAsFloat(const BYTE* frame, UINT32 channel, const WAVEFORMATEX* format) {
    if (!frame || !format || channel >= format->nChannels) return 0.0f;
    const UINT32 bytesPerSample = format->nBlockAlign / format->nChannels;
    const BYTE* p = frame + static_cast<size_t>(channel) * bytesPerSample;

    if (IsFloatFormat(format) && format->wBitsPerSample == 32 && bytesPerSample == 4) {
        return *reinterpret_cast<const float*>(p);
    }

    if (!IsPcmFormat(format)) return 0.0f;

    if (bytesPerSample == 2 && format->wBitsPerSample == 16) {
        const auto v = *reinterpret_cast<const int16_t*>(p);
        return static_cast<float>(v) / 32768.0f;
    }
    if (bytesPerSample == 3 && format->wBitsPerSample == 24) {
        int32_t v = static_cast<int32_t>(p[0]) |
                    (static_cast<int32_t>(p[1]) << 8) |
                    (static_cast<int32_t>(p[2]) << 16);
        if (v & 0x00800000) v |= ~0x00FFFFFF;
        return static_cast<float>(v) / 8388608.0f;
    }
    if (bytesPerSample == 4 && format->wBitsPerSample == 32) {
        const auto v = *reinterpret_cast<const int32_t*>(p);
        return static_cast<float>(static_cast<double>(v) / 2147483648.0);
    }
    return 0.0f;
}

void WriteStereoFrame(BYTE* frame, const WAVEFORMATEX* format, float left, float right) {
    if (!frame || !format || format->nChannels != 2) return;
    left = ClampUnit(left);
    right = ClampUnit(right);
    const UINT32 bytesPerSample = format->nBlockAlign / 2;

    if (IsFloatFormat(format) && format->wBitsPerSample == 32 && bytesPerSample == 4) {
        auto* dst = reinterpret_cast<float*>(frame);
        dst[0] = left;
        dst[1] = right;
        return;
    }

    if (!IsPcmFormat(format)) return;

    auto writeOne = [bytesPerSample, format](BYTE* p, float value) {
        value = std::clamp(value, -1.0f, 0.99999994f);
        if (bytesPerSample == 2 && format->wBitsPerSample == 16) {
            const int v = static_cast<int>(std::lrint(value * 32767.0f));
            *reinterpret_cast<int16_t*>(p) = static_cast<int16_t>(std::clamp(v, -32768, 32767));
        } else if (bytesPerSample == 3 && format->wBitsPerSample == 24) {
            const int32_t v = static_cast<int32_t>(std::lrint(value * 8388607.0f));
            p[0] = static_cast<BYTE>(v & 0xFF);
            p[1] = static_cast<BYTE>((v >> 8) & 0xFF);
            p[2] = static_cast<BYTE>((v >> 16) & 0xFF);
        } else if (bytesPerSample == 4 && format->wBitsPerSample == 32) {
            const int64_t v = static_cast<int64_t>(std::llround(value * 2147483647.0));
            *reinterpret_cast<int32_t*>(p) = static_cast<int32_t>(std::clamp<int64_t>(v, INT32_MIN, INT32_MAX));
        }
    };

    writeOne(frame, left);
    writeOne(frame + bytesPerSample, right);
}

std::vector<DWORD> BuildSpeakerOrder(const WAVEFORMATEX* format) {
    std::vector<DWORD> order;
    if (!format) return order;
    const DWORD mask = ChannelMaskForFormat(format);
    if (mask != 0) {
        for (DWORD bit = 1; bit != 0 && order.size() < format->nChannels; bit <<= 1) {
            if (mask & bit) order.push_back(bit);
        }
    }

    while (order.size() < format->nChannels) {
        // Unknown / unmasked extra channel.
        order.push_back(0);
    }
    return order;
}

StereoFrame DownmixFrame(const BYTE* frame,
                         const WAVEFORMATEX* format,
                         const std::vector<DWORD>& speakerOrder,
                         float& limiterGain) {
    StereoFrame out{};
    if (!format) return out;

    if (format->nChannels == 1) {
        const float s = ReadSampleAsFloat(frame, 0, format);
        out.l = s;
        out.r = s;
    } else if (format->nChannels == 2) {
        // Clean capture is already stereo. Preserve it bit-for-bit in float space.
        out.l = ReadSampleAsFloat(frame, 0, format);
        out.r = ReadSampleAsFloat(frame, 1, format);
    } else {
        for (UINT32 ch = 0; ch < format->nChannels; ++ch) {
            const float sample = ReadSampleAsFloat(frame, ch, format);
            const DWORD speaker = ch < speakerOrder.size() ? speakerOrder[ch] : 0;
            AddSpeakerContribution(speaker, sample, out);
        }
    }

    // Emergency linked peak guard only. The fixed-headroom matrix keeps it inactive
    // for valid 7.1 full-scale channel combinations; it catches malformed/hot input.
    const float peak = std::max(std::abs(out.l), std::abs(out.r));
    const float desired = peak > kLimiterCeiling ? (kLimiterCeiling / peak) : 1.0f;
    if (desired < limiterGain) {
        limiterGain = desired;
    } else {
        limiterGain += (1.0f - limiterGain) * 0.002f;
        limiterGain = std::min(limiterGain, 1.0f);
    }
    out.l *= limiterGain;
    out.r *= limiterGain;
    return out;
}

WAVEFORMATEXTENSIBLE MakeStereo48kFloatFormat() {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = 2;
    f.Format.nSamplesPerSec = 48000;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = 2 * sizeof(float);
    f.Format.nAvgBytesPerSec = f.Format.nSamplesPerSec * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return f;
}

} // namespace lm
