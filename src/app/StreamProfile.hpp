#pragma once

#include <span>
#include <string_view>

namespace veloplay {

enum class VideoCodec { Hevc, H264 };

inline constexpr int kMinBitrateKbps = 5'000;
inline constexpr int kMaxBitrateKbps = 60'000;

/// What the client asks the console to send.
struct StreamSettings {
    int width = 1920;
    int height = 1080;
    int fps = 30; // 30 or 60
    VideoCodec codec = VideoCodec::Hevc;
    bool hdr = false; // needs HEVC
    // 15 Mbit/s is what the official client uses for 1080p. A console on
    // Wi-Fi often cannot send much more without losing packets.
    int bitrateKbps = 15'000;

    friend bool operator==(const StreamSettings&, const StreamSettings&) = default;
};

/// A named starting point for the stream and frame generation settings.
struct StreamProfile {
    std::string_view id;
    StreamSettings stream;
    int frameGenerationFactor; // 0 = automatic, 1 = off, 2..4 = output frames per stream frame
};

/// 30 fps from the console, interpolated to 60 fps on the PC. For games that
/// run at 30 fps on the console: every stream frame is a real game frame, and
/// each one gets twice the bits of a 60 fps stream.
inline constexpr std::string_view kProfileSmooth30 = "smooth30";

/// 60 fps from the console, shown as is. For games that run at 60 fps.
inline constexpr std::string_view kProfileNative60 = "native60";

/// Settings changed by hand.
inline constexpr std::string_view kProfileCustom = "custom";

std::span<const StreamProfile> builtInProfiles();
const StreamProfile* findProfile(std::string_view id);

/// Clamps values to what the console accepts.
void sanitize(StreamSettings& settings);

} // namespace veloplay
