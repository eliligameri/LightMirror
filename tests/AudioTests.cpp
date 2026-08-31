#include "AudioUtils.h"
#include "StereoRingBuffer.h"

#include <cmath>
#include <iostream>

namespace {

bool Near(float a, float b, float eps = 0.0005f) {
    return std::abs(a - b) <= eps;
}

int Fail(const char* message) {
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

} // namespace

int main() {
    WAVEFORMATEXTENSIBLE source{};
    source.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    source.Format.nChannels = 8;
    source.Format.nSamplesPerSec = 48000;
    source.Format.wBitsPerSample = 32;
    source.Format.nBlockAlign = 8 * sizeof(float);
    source.Format.nAvgBytesPerSec = source.Format.nSamplesPerSec * source.Format.nBlockAlign;
    source.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    source.Samples.wValidBitsPerSample = 32;
    source.dwChannelMask = KSAUDIO_SPEAKER_7POINT1_SURROUND;
    source.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

    const auto order = lm::BuildSpeakerOrder(&source.Format);
    if (order.size() != 8 || order[0] != SPEAKER_FRONT_LEFT || order[1] != SPEAKER_FRONT_RIGHT ||
        order[2] != SPEAKER_FRONT_CENTER || order[3] != SPEAKER_LOW_FREQUENCY ||
        order[4] != SPEAKER_BACK_LEFT || order[5] != SPEAKER_BACK_RIGHT ||
        order[6] != SPEAKER_SIDE_LEFT || order[7] != SPEAKER_SIDE_RIGHT) {
        return Fail("7.1 channel-mask ordering is wrong");
    }

    float channels[8]{};
    float limiter = 1.0f;

    channels[2] = 0.5f; // center only
    auto c = lm::DownmixFrame(reinterpret_cast<const BYTE*>(channels), &source.Format, order, limiter);
    if (!Near(c.l, 0.125f) || !Near(c.r, 0.125f)) return Fail("center downmix coefficient");

    for (float& sample : channels) sample = 0.0f;
    limiter = 1.0f;
    channels[3] = 0.5f; // LFE only
    auto lfe = lm::DownmixFrame(reinterpret_cast<const BYTE*>(channels), &source.Format, order, limiter);
    if (!Near(lfe.l, 0.02f) || !Near(lfe.r, 0.02f)) return Fail("LFE downmix coefficient");

    for (float& sample : channels) sample = 1.0f;
    limiter = 1.0f;
    auto hot = lm::DownmixFrame(reinterpret_cast<const BYTE*>(channels), &source.Format, order, limiter);
    if (!Near(hot.l, 0.99f) || !Near(hot.r, 0.99f) || limiter != 1.0f) {
        return Fail("7.1 fixed headroom should not engage limiter");
    }

    WAVEFORMATEX stereoInput{};
    stereoInput.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    stereoInput.nChannels = 2;
    stereoInput.nSamplesPerSec = 48000;
    stereoInput.wBitsPerSample = 32;
    stereoInput.nBlockAlign = 8;
    float stereoSamples[2]{0.75f, -0.5f};
    limiter = 1.0f;
    auto passthrough = lm::DownmixFrame(reinterpret_cast<const BYTE*>(stereoSamples),
                                        &stereoInput, lm::BuildSpeakerOrder(&stereoInput), limiter);
    if (!Near(passthrough.l, 0.75f) || !Near(passthrough.r, -0.5f)) {
        return Fail("stereo clean-capture passthrough");
    }

    const auto stereo = lm::MakeStereo48kFloatFormat();
    if (stereo.Format.nChannels != 2 || stereo.Format.nSamplesPerSec != 48000 ||
        !lm::IsFloatFormat(&stereo.Format)) {
        return Fail("48 kHz stereo output format");
    }

    lm::StereoRingBuffer ring(4);
    if (!ring.Push({1.0f, 2.0f}) || !ring.Push({3.0f, 4.0f})) return Fail("ring push");
    auto first = ring.Peek(0);
    if (!Near(first.l, 1.0f) || !Near(first.r, 2.0f)) return Fail("ring peek");
    ring.Pop(1);
    first = ring.Peek(0);
    if (!Near(first.l, 3.0f) || ring.Size() != 1) return Fail("ring pop");

    std::cout << "LightMirror audio tests passed.\n";
    return 0;
}
