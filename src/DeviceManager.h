#pragma once

#include "AudioTypes.h"

#include <string>
#include <vector>

namespace lm {

class DeviceManager {
public:
    static std::vector<DeviceInfo> EnumerateActiveRenderDevices();
    static std::wstring DefaultRenderDeviceId();
};

} // namespace lm
