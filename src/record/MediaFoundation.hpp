#pragma once

#include <string>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

namespace veloplay::record {

/// Media Foundation, loaded when first asked for. Windows N editions and
/// some servers come without it, and a program that names its libraries at
/// load time would not start there at all. So nothing links them; the few
/// functions needed are looked up here, once.
struct MediaFoundation {
    decltype(&::MFStartup) startup = nullptr;
    decltype(&::MFShutdown) shutdown = nullptr;
    decltype(&::MFCreateAttributes) createAttributes = nullptr;
    decltype(&::MFCreateMediaType) createMediaType = nullptr;
    decltype(&::MFCreateSample) createSample = nullptr;
    decltype(&::MFCreateMemoryBuffer) createMemoryBuffer = nullptr;
    decltype(&::MFCreateSinkWriterFromURL) createSinkWriterFromUrl = nullptr;
    decltype(&::MFCreateSourceReaderFromURL) createSourceReaderFromUrl = nullptr;

    /// The functions, or null with the reason in `why` if this PC has no Media Foundation.
    static const MediaFoundation* get(std::string* why = nullptr);
};

} // namespace veloplay::record
