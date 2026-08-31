#include "ProcessLoopback.h"

#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <wrl/implements.h>

namespace lm {
namespace {

class ActivationHandler final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          IActivateAudioInterfaceCompletionHandler> {
public:
    explicit ActivationHandler(HANDLE completed) : completed_(completed) {}

    IFACEMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        Microsoft::WRL::ComPtr<IUnknown> activated;
        HRESULT activationResult = E_FAIL;
        HRESULT hr = operation->GetActivateResult(&activationResult, &activated);
        result_ = FAILED(hr) ? hr : activationResult;
        if (SUCCEEDED(result_)) result_ = activated.As(&audioClient_);
        SetEvent(completed_);
        return S_OK;
    }

    HRESULT Result() const { return result_; }
    Microsoft::WRL::ComPtr<IAudioClient> Client() const { return audioClient_; }

private:
    HANDLE completed_ = nullptr;
    HRESULT result_ = E_PENDING;
    Microsoft::WRL::ComPtr<IAudioClient> audioClient_;
};

} // namespace

HRESULT ActivateCleanSystemLoopback(Microsoft::WRL::ComPtr<IAudioClient>& audioClient,
                                    HANDLE stopEvent) {
    audioClient.Reset();
    HANDLE completed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!completed) return HRESULT_FROM_WIN32(GetLastError());

    auto handler = Microsoft::WRL::Make<ActivationHandler>(completed);
    if (!handler) {
        CloseHandle(completed);
        return E_OUTOFMEMORY;
    }

    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    params.ProcessLoopbackParams.ProcessLoopbackMode =
        PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

    PROPVARIANT activation{};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    Microsoft::WRL::ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                              __uuidof(IAudioClient), &activation,
                                              handler.Get(), &operation);
    if (SUCCEEDED(hr)) {
        HANDLE waits[2] = {stopEvent, completed};
        const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, 10000);
        if (wait == WAIT_OBJECT_0 + 1) {
            hr = handler->Result();
            if (SUCCEEDED(hr)) audioClient = handler->Client();
        } else if (wait == WAIT_OBJECT_0) {
            hr = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        } else if (wait == WAIT_TIMEOUT) {
            hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        } else {
            hr = HRESULT_FROM_WIN32(GetLastError());
        }
    }

    // The operation owns the callback until completion. Join that callback before
    // closing its event even if startup was cancelled or timed out.
    if (operation && WaitForSingleObject(completed, 0) != WAIT_OBJECT_0) {
        WaitForSingleObject(completed, INFINITE);
    }
    operation.Reset();
    CloseHandle(completed);
    return hr;
}

} // namespace lm
