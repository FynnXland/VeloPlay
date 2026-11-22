#include "record/MediaFoundation.hpp"

#include <windows.h>

#include <mutex>

namespace veloplay::record {

namespace {

template <class Function>
bool lookUp(HMODULE module, const char* name, Function& into) {
    into = reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    return into != nullptr;
}

} // namespace

const MediaFoundation* MediaFoundation::get(std::string* why) {
    static MediaFoundation functions;
    static std::string failure;
    static std::once_flag once;
    std::call_once(once, [] {
        // From the system folder only: a file of that name next to the program must not be picked up.
        const HMODULE platform = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const HMODULE readWrite = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!platform || !readWrite) {
            failure = "this PC has no Media Foundation (Windows N editions need the Media Feature Pack)";
            return;
        }
        const bool all = lookUp(platform, "MFStartup", functions.startup) &&
                         lookUp(platform, "MFShutdown", functions.shutdown) &&
                         lookUp(platform, "MFCreateAttributes", functions.createAttributes) &&
                         lookUp(platform, "MFCreateMediaType", functions.createMediaType) &&
                         lookUp(platform, "MFCreateSample", functions.createSample) &&
                         lookUp(platform, "MFCreateMemoryBuffer", functions.createMemoryBuffer) &&
                         lookUp(readWrite, "MFCreateSinkWriterFromURL", functions.createSinkWriterFromUrl) &&
                         lookUp(readWrite, "MFCreateSourceReaderFromURL", functions.createSourceReaderFromUrl);
        if (!all) failure = "Media Foundation on this PC lacks functions VeloPlay needs";
    });
    if (!failure.empty()) {
        if (why) *why = failure;
        return nullptr;
    }
    return &functions;
}

} // namespace veloplay::record
