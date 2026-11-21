#include "audio/WasapiOutput.hpp"

#include <chrono>
#include <future>
#include <string>

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

/// The name Windows itself knows an output by; it tells two devices apart.
std::wstring deviceId(IMMDevice* device) {
    std::wstring id;
    LPWSTR raw = nullptr;
    if (device && SUCCEEDED(device->GetId(&raw)) && raw) {
        id = raw;
        CoTaskMemFree(raw);
    }
    return id;
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
            // The default output is followed: when Windows moves it (headphones
            // plugged in, a headset switched on) or the device goes away, the
            // sound is opened again on whatever is the default then. A device
            // picked by name, the controller's, is opened once; without it the
            // haptics fall back to plain rumble elsewhere.
            const bool follow = needles_.empty();
            ComPtr<IMMDeviceEnumerator> enumerator;
            HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            const bool usable = wake != nullptr &&
                                SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                                           IID_PPV_ARGS(enumerator.GetAddressOf())));
            bool stopping = !usable;
            bool silentLogged = false;
            while (!stopping) {
                ComPtr<IMMDevice> device;
                ComPtr<IAudioClient> client;
                ComPtr<IAudioRenderClient> render;
                UINT32 capacity = 0;
                device = findDevice(enumerator.Get(), needles_);
                bool ok = device != nullptr;
                const std::wstring openedId = ok ? deviceId(device.Get()) : std::wstring{};
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
                                           : channels == 4 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT |
                                                              SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT)
                                                           : 0;
                    format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
                    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
                    ok = SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kDeviceBuffer, 0,
                                                      &format.Format, nullptr)) &&
                         SUCCEEDED(client->SetEventHandle(wake)) && SUCCEEDED(client->GetBufferSize(&capacity)) &&
                         SUCCEEDED(client->GetService(IID_PPV_ARGS(render.GetAddressOf()))) &&
                         SUCCEEDED(client->Start());
                }

                running_.store(ok);
                if (!reported) {
                    promise.set_value(ok);
                    reported = true;
                }
                if (ok) {
                    spdlog::info("Sound output open: {} Hz, {} channels{}", sampleRate, channels,
                                 needles_.empty() ? "" : " (controller)");
                    silentLogged = false;
                } else if (follow && !silentLogged) {
                    spdlog::warn("No sound device could be opened; the stream stays silent until there is one");
                    silentLogged = true;
                }
                if (!ok) {
                    if (!follow) break;
                    // Nothing to play on right now. Look again in a moment.
                    if (WaitForSingleObject(static_cast<HANDLE>(stopEvent_), 1000) != WAIT_TIMEOUT) break;
                    continue;
                }

                DWORD taskIndex = 0;
                const HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
                const HANDLE handles[2] = {static_cast<HANDLE>(stopEvent_), wake};
                auto lastLook = std::chrono::steady_clock::now();
                while (true) {
                    const DWORD woke = WaitForMultipleObjects(2, handles, FALSE, 500);
                    if (woke == WAIT_OBJECT_0 || woke == WAIT_FAILED) {
                        stopping = true;
                        break;
                    }
                    // Once a second: is this still the device Windows plays on by default?
                    const auto now = std::chrono::steady_clock::now();
                    if (follow && now - lastLook >= std::chrono::seconds(1)) {
                        lastLook = now;
                        ComPtr<IMMDevice> current;
                        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, current.GetAddressOf())) &&
                            current && deviceId(current.Get()) != openedId) {
                            spdlog::info("Windows plays on another sound device now; the stream's sound moves there");
                            break;
                        }
                    }
                    UINT32 padding = 0;
                    if (FAILED(client->GetCurrentPadding(&padding))) {
                        // Unplugged or reconfigured.
                        spdlog::warn("The sound device is gone{}", follow ? "; looking for another one" : "");
                        break;
                    }
                    const UINT32 frames = capacity - padding;
                    if (frames == 0) continue;
                    BYTE* data = nullptr;
                    if (FAILED(render->GetBuffer(frames, &data))) {
                        spdlog::warn("The sound device is gone{}", follow ? "; looking for another one" : "");
                        break;
                    }
                    {
                        std::lock_guard lock(mutex_);
                        buffer_->pop({reinterpret_cast<std::int16_t*>(data), static_cast<std::size_t>(frames) *
                                                                                static_cast<std::size_t>(channels)});
                    }
                    render->ReleaseBuffer(frames, 0);
                }
                client->Stop();
                if (task) AvRevertMmThreadCharacteristics(task);
                running_.store(false);
                if (!follow) break;
                // A device that just vanished can take a moment to leave the list.
                if (!stopping && WaitForSingleObject(static_cast<HANDLE>(stopEvent_), 200) != WAIT_TIMEOUT) break;
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
