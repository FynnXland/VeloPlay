#include "audio/WasapiInput.hpp"

#include <chrono>
#include <string>
#include <vector>

#include <windows.h>

#include <mmdeviceapi.h>

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

constexpr REFERENCE_TIME kDeviceBuffer = 20 * 10'000; // 20 ms in 100 ns units
// How long to wait before looking for a microphone again when there is none.
constexpr DWORD kLookAgainMs = 2000;

std::wstring deviceId(IMMDevice* device) {
    std::wstring id;
    LPWSTR raw = nullptr;
    if (device && SUCCEEDED(device->GetId(&raw)) && raw) {
        id = raw;
        CoTaskMemFree(raw);
    }
    return id;
}

/// The microphone Windows uses for calls, or failing that the general default.
ComPtr<IMMDevice> defaultMicrophone(IMMDeviceEnumerator* enumerator) {
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eCapture, eCommunications, device.GetAddressOf())) || !device) {
        device.Reset();
        enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, device.GetAddressOf());
    }
    return device;
}

} // namespace

WasapiInput::WasapiInput() { stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }

WasapiInput::~WasapiInput() {
    stop();
    if (stopEvent_) CloseHandle(static_cast<HANDLE>(stopEvent_));
}

bool WasapiInput::start(Sink sink) {
    stop();
    if (!stopEvent_ || !sink) return false;
    ResetEvent(static_cast<HANDLE>(stopEvent_));
    running_.store(true);

    // The device is opened on the thread that reads it: opening can take a
    // while, and the caller is the thread that draws the window.
    thread_ = std::thread([this, sink = std::move(sink)] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        {
            ComPtr<IMMDeviceEnumerator> enumerator;
            HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            const bool usable = wake != nullptr &&
                                SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                                           IID_PPV_ARGS(enumerator.GetAddressOf())));
            const std::size_t blockSamples = static_cast<std::size_t>(media::kMicFrameSamples * media::kMicChannels);
            std::vector<std::int16_t> pending; // recorded and not yet handed on
            pending.reserve(blockSamples * 8);
            bool stopping = !usable;
            bool missingLogged = false;
            while (!stopping) {
                ComPtr<IMMDevice> device = defaultMicrophone(enumerator.Get());
                ComPtr<IAudioClient> client;
                ComPtr<IAudioCaptureClient> capture;
                bool ok = device != nullptr;
                const std::wstring openedId = ok ? deviceId(device.Get()) : std::wstring{};
                ok = ok && SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                                      reinterpret_cast<void**>(client.GetAddressOf())));
                if (ok) {
                    WAVEFORMATEXTENSIBLE format{};
                    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
                    format.Format.nChannels = static_cast<WORD>(media::kMicChannels);
                    format.Format.nSamplesPerSec = static_cast<DWORD>(media::kMicSampleRate);
                    format.Format.wBitsPerSample = 16;
                    format.Format.nBlockAlign = static_cast<WORD>(media::kMicChannels * 2);
                    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
                    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
                    format.Samples.wValidBitsPerSample = 16;
                    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
                    format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
                    // Windows turns a mono or 44.1 kHz microphone into what is asked for here.
                    const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
                    ok = SUCCEEDED(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kDeviceBuffer, 0,
                                                      &format.Format, nullptr)) &&
                         SUCCEEDED(client->SetEventHandle(wake)) &&
                         SUCCEEDED(client->GetService(IID_PPV_ARGS(capture.GetAddressOf()))) &&
                         SUCCEEDED(client->Start());
                }
                if (!ok) {
                    if (!missingLogged) {
                        spdlog::warn("No microphone could be opened; looking again every {} ms. Windows may also keep "
                                     "desktop programs from the microphone (Settings > Privacy > Microphone).",
                                     kLookAgainMs);
                        missingLogged = true;
                    }
                    if (WaitForSingleObject(static_cast<HANDLE>(stopEvent_), kLookAgainMs) != WAIT_TIMEOUT) break;
                    continue;
                }
                missingLogged = false;
                pending.clear();
                recording_.store(true);
                spdlog::info("Microphone open: {} Hz, {} channels", media::kMicSampleRate, media::kMicChannels);

                DWORD taskIndex = 0;
                const HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);
                const HANDLE handles[2] = {static_cast<HANDLE>(stopEvent_), wake};
                auto lastLook = std::chrono::steady_clock::now();
                bool deviceLost = false;
                while (!deviceLost) {
                    const DWORD woke = WaitForMultipleObjects(2, handles, FALSE, 500);
                    if (woke == WAIT_OBJECT_0 || woke == WAIT_FAILED) {
                        stopping = true;
                        break;
                    }
                    const auto now = std::chrono::steady_clock::now();
                    if (now - lastLook >= std::chrono::seconds(1)) {
                        lastLook = now;
                        ComPtr<IMMDevice> current = defaultMicrophone(enumerator.Get());
                        if (current && deviceId(current.Get()) != openedId) {
                            spdlog::info("Windows uses another microphone now; recording moves there");
                            break;
                        }
                    }
                    // Everything the device has collected since the last time.
                    while (true) {
                        UINT32 next = 0;
                        if (FAILED(capture->GetNextPacketSize(&next))) {
                            deviceLost = true;
                            break;
                        }
                        if (next == 0) break;
                        BYTE* data = nullptr;
                        UINT32 frames = 0;
                        DWORD bufferFlags = 0;
                        if (FAILED(capture->GetBuffer(&data, &frames, &bufferFlags, nullptr, nullptr))) {
                            deviceLost = true;
                            break;
                        }
                        const std::size_t samples = static_cast<std::size_t>(frames) * media::kMicChannels;
                        if ((bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || !data) {
                            pending.insert(pending.end(), samples, 0);
                        } else {
                            const auto* pcm = reinterpret_cast<const std::int16_t*>(data);
                            pending.insert(pending.end(), pcm, pcm + samples);
                        }
                        capture->ReleaseBuffer(frames);
                    }
                    std::size_t done = 0;
                    while (pending.size() - done >= blockSamples) {
                        sink(std::span<const std::int16_t>(pending.data() + done, blockSamples));
                        done += blockSamples;
                    }
                    if (done > 0) pending.erase(pending.begin(), pending.begin() + static_cast<std::ptrdiff_t>(done));
                }
                if (deviceLost) spdlog::warn("The microphone is gone; looking for another one");
                client->Stop();
                if (task) AvRevertMmThreadCharacteristics(task);
                recording_.store(false);
                // A device that just vanished can take a moment to leave the list.
                if (!stopping && WaitForSingleObject(static_cast<HANDLE>(stopEvent_), 200) != WAIT_TIMEOUT) break;
            }
            if (wake) CloseHandle(wake);
        }
        recording_.store(false);
        running_.store(false);
        if (SUCCEEDED(com)) CoUninitialize();
    });
    return true;
}

void WasapiInput::stop() {
    if (thread_.joinable()) {
        SetEvent(static_cast<HANDLE>(stopEvent_));
        thread_.join();
    }
    running_.store(false);
    recording_.store(false);
}

} // namespace veloplay::audio
