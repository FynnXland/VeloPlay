#include "app/StreamProfile.hpp"

#include <algorithm>
#include <array>

namespace veloplay {

namespace {

constexpr std::array<StreamProfile, 2> kProfiles = {{
    // Frame generation on automatic in both: it doubles 30 fps and leaves 60 fps alone.
    {kProfileSmooth30, StreamSettings{1920, 1080, 30, VideoCodec::Hevc, false, 15'000}, 0},
    {kProfileNative60, StreamSettings{1920, 1080, 60, VideoCodec::Hevc, false, 20'000}, 0},
}};

} // namespace

std::span<const StreamProfile> builtInProfiles() { return kProfiles; }

const StreamProfile* findProfile(std::string_view id) {
    for (const StreamProfile& profile : kProfiles) {
        if (profile.id == id) return &profile;
    }
    return nullptr;
}

void sanitize(StreamSettings& settings) {
    // The console offers fixed 16:9 modes. Anything else falls back to 1080p.
    const bool knownMode = (settings.width == 1920 && settings.height == 1080) ||
                           (settings.width == 1280 && settings.height == 720);
    if (!knownMode) {
        settings.width = 1920;
        settings.height = 1080;
    }
    settings.fps = settings.fps >= 45 ? 60 : 30;
    settings.bitrateKbps = std::clamp(settings.bitrateKbps, kMinBitrateKbps, kMaxBitrateKbps);
    if (settings.codec != VideoCodec::Hevc) settings.hdr = false;
}

} // namespace veloplay
