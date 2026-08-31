#pragma once

#include "AudioTypes.h"

#include <audioclient.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <string>
#include <vector>

namespace lm {

std::wstring HResultToString(HRESULT hr);
std::wstring FormatAudioFormat(const WAVEFORMATEX* format);

bool IsFloatFormat(const WAVEFORMATEX* format);
bool IsPcmFormat(const WAVEFORMATEX* format);
DWORD ChannelMaskForFormat(const WAVEFORMATEX* format);

float ReadSampleAsFloat(const BYTE* frame, UINT32 channel, const WAVEFORMATEX* format);
void WriteStereoFrame(BYTE* frame, const WAVEFORMATEX* format, float left, float right);

std::vector<DWORD> BuildSpeakerOrder(const WAVEFORMATEX* format);
StereoFrame DownmixFrame(const BYTE* frame,
                         const WAVEFORMATEX* format,
                         const std::vector<DWORD>& speakerOrder,
                         float& limiterGain);

WAVEFORMATEXTENSIBLE MakeStereo48kFloatFormat();

} // namespace lm
