#include "DeviceManager.h"

#include <windows.h>
#include <propkeydef.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propvarutil.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace lm {

std::vector<DeviceInfo> DeviceManager::EnumerateActiveRenderDevices() {
    std::vector<DeviceInfo> result;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        return result;
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection))) {
        return result;
    }

    UINT count = 0;
    if (FAILED(collection->GetCount(&count))) return result;

    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(i, &device))) continue;

        LPWSTR rawId = nullptr;
        if (FAILED(device->GetId(&rawId)) || !rawId) continue;
        std::wstring id(rawId);
        CoTaskMemFree(rawId);

        std::wstring name = L"Audio device";
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR && pv.pwszVal) {
                name = pv.pwszVal;
            }
            PropVariantClear(&pv);
        }
        result.push_back({std::move(id), std::move(name), true});
    }
    return result;
}

std::wstring DeviceManager::DefaultRenderDeviceId() {
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        return {};
    }
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device))) return {};
    LPWSTR rawId = nullptr;
    if (FAILED(device->GetId(&rawId)) || !rawId) return {};
    std::wstring id(rawId);
    CoTaskMemFree(rawId);
    return id;
}

} // namespace lm
