#pragma once

#include <audioclient.h>
#include <windows.h>
#include <wrl/client.h>

namespace lm {

// Opens the endpoint-independent process-loopback virtual device and excludes
// LightMirror itself so its HDMI render stream cannot feed back into capture.
HRESULT ActivateCleanSystemLoopback(Microsoft::WRL::ComPtr<IAudioClient>& audioClient,
                                    HANDLE stopEvent);

} // namespace lm
