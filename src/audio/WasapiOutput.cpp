#include "audio/WasapiOutput.hpp"

#include <future>

#include <windows.h>

// The order matters here. initguid.h makes this file the one that defines
// PKEY_Device_FriendlyName instead of only declaring it, and the property key
// header needs the macros from propkeydef.h, which a lean windows.h leaves out.
#include <initguid.h>

#include <mmdeviceapi.h>
#include <propidl.h>
#include <propkeydef.h>

#include <functiondiscoverykeys_devpkey.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmreg.h>

#include <ks.h>

#include <ksmedia.h>
#include <wrl/client.h>

#include <spdlog/spdlog.h>

namespace veloplay::audio {

using Microsoft::WRL::ComPtr;

namespace {

// How much the device itself buffers. The reserve against late packets lives
// in AudioBuffer, so this only has to cover the wake-up of the thread.
constexpr REFERENCE_TIME kDeviceBuffer = 20 * 10'000; // 20 ms in 100 ns units
// Audio older than this beyond the reserve is dropped instead of played late.
constexpr std::chrono::milliseconds kExtraBeforeDrop{80};

ComPtr<IMMDevice> findDevice(IMMDeviceEnumerator* enumerator, const std::vector<std::wstring>& needles) {
    ComPtr<IMMDevice> device;
    if (needles.empty()) {
        enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device.GetAddressOf());
        return device;
    }
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devices.GetAddressOf()))) return device;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> candidate;
        ComPtr<IPropertyStore> properties;
        if (FAILED(devices->Item(i, candidate.GetAddressOf()))) continue;
        if (FAILED(candidate->OpenPropertyStore(STGM_READ, properties.GetAddressOf()))) continue;
        PROPVARIANT name;
        PropVariantInit(&name);
        if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR && name.pwszVal) {
            for (const std::wstring& needle : needles) {
                if (wcsstr(name.pwszVal, needle.c_str())) device = candidate;
            }
        }
        PropVariantClear(&name);
        if (device) break;
    }
    return device;
}

} // namespace

WasapiOutput::WasapiOutput(std::vector<std::wstring> deviceNameNeedles) : needles_(std::move(deviceNameNeedles)) {
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

WasapiOutput::~WasapiOutput() {
    stop();
    if (stopEvent_) CloseHandle(static_cast<HANDLE>(stopEvent_));
}

bool WasapiOutput::start(int sampleRate, int channels, std::chrono::milliseconds delay) {
    stop();
    if (!stopEvent_ || sampleRate <= 0 || channels <= 0) return false;
    {
        std::lock_guard lock(mutex_);
        buffer_ = std::make_unique<media::AudioBuffer>(sampleRate, channels, delay, delay + kExtraBeforeDrop);
    }
    ResetEvent(static_cast<HANDLE>(stopEvent_));

    // The device is opened on the thread that feeds it. start() does not wait:
    // it is called from the network thread, and opening can take a while.
    std::promise<bool> opened;
    opened_ = opened.get_future();
    thread_ = std::thread([this, sampleRate, channels, promise = std::move(opened)]() mutable {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool reported = false;
        {
            ComPtr<IMMDeviceEnumerator> enumerator;
            ComPtr<IMMDevice> device;
            ComPtr<IAudioClient> client;
            ComPtr<IAudioRenderClient> render;
            HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            UINT32 capacity = 0;
            bool ok = wake != nullptr &&
                      SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                                 IID_PPV_ARGS(enumerator.GetAddressOf())));
            if (ok) {
                device = findDevice(enumerator.Get(), needles_);
                ok = device != nullptr;
            }
            ok = ok && SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                  reinterpret_cast<void**>(client.GetAddressOf())));
            if (ok && !needles_.empty()) {
                // A device picked by name must really have the channels, or
                // Windows would mix the haptics down into the speaker.
                WAVEFORMATEX* mix = nullptr;
                ok = SUCCEEDED(client->GetMixFormat(&mix)) && mix && mix->nChannels >= channels;
                if (mix && mix->nChannels < channels) {
                    spdlog::warn("The controller's sound device is set to {} channels, {} are needed. Set it to "
                                 "quadraphonic in the Windows sound settings to get haptics.",
                                 mix->nChannels, channels);
                }
                if (mix) CoTaskMemFree(mix);
            }
            if (ok) {
                WAVEFORMATEXTENSIBLE format{};
                format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                format.Format.nChannels = static_cast<WORD>(channels);
                format.Format.nSamplesPerSec = static_cast<DWORD>(sampleRate);
                format.Format.wBitsPerSample = 16;
                format.Format.nBlockAlign = static_cast<WORD>(channels * 2);
                format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
                format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                format.Samples.wValidBitsPerSample = 16;
                format.dwChannelMask = channels == 1   ? SPEAKER_FRONT_CENTER
                                       : channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                       : channels == 4 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_BACK_LEFT |
                                                          SPEAKER_BACK_RIGHT)
                                                       : 0;
                format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
                const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
                ok = SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kDeviceBuffer, 0, &format.Format,
                                                  nullptr)) &&
                     SUCCEEDED(client->SetEventHandle(wake)) && SUCCEEDED(client->GetBufferSize(&capacity)) &&
                     SUCCEEDED(client->GetService(IID_PPV_ARGS(render.GetAddressOf()))) && SUCCEEDED(client->Start());
            }

            running_.store(ok);
            promise.set_value(ok);
            reported = true;
            if (ok) {
                spdlog::info("Sound output open: {} Hz, {} channels{}", sampleRate, channels,
                             needles_.empty() ? "" : " (controller)");
            } else if (needles_.empty()) {
                spdlog::warn("No sound device could be opened; the stream stays silent");
            }

            if (ok) {
                DWORD taskIndex = 0;
                const HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
                const HANDLE handles[2] = {static_cast<HANDLE>(stopEvent_), wake};
                while (true) {
                    const DWORD woke = WaitForMultipleObjects(2, handles, FALSE, 500);
                    if (woke == WAIT_OBJECT_0 || woke == WAIT_FAILED) break;
                    UINT32 padding = 0;
                    if (FAILED(client->GetCurrentPadding(&padding))) break; // device unplugged or reconfigured
                    const UINT32 frames = capacity - padding;
                    if (frames == 0) continue;
                    BYTE* data = nullptr;
                    if (FAILED(render->GetBuffer(frames, &data))) break;
                    {
                        std::lock_guard lock(mutex_);
                        buffer_->pop({reinterpret_cast<std::int16_t*>(data), static_cast<std::size_t>(frames) *
                                                                                static_cast<std::size_t>(channels)});
                    }
                    render->ReleaseBuffer(frames, 0);
                }
                client->Stop();
                if (task) AvRevertMmThreadCharacteristics(task);
            }
            if (wake) CloseHandle(wake);
        }
        if (!reported) promise.set_value(false);
        running_.store(false);
        if (SUCCEEDED(com)) CoUninitialize();
    });

    return true;
}

bool WasapiOutput::waitUntilOpened(std::chrono::milliseconds timeout) {
    if (!opened_.valid()) return false;
    if (opened_.wait_for(timeout) != std::future_status::ready) return false;
    return running_.load();
}

void WasapiOutput::write(std::span<const std::int16_t> interleaved) {
    if (!running_.load()) return;
    std::lock_guard lock(mutex_);
    if (buffer_) buffer_->push(interleaved);
}

void WasapiOutput::stop() {
    if (thread_.joinable()) {
        SetEvent(static_cast<HANDLE>(stopEvent_));
        thread_.join();
    }
    running_.store(false);
    std::lock_guard lock(mutex_);
    buffer_.reset();
}

} // namespace veloplay::audio
