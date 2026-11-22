#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace veloplay {

/// A capture card shows up twice in Windows: as a video source and as a
/// sound input, each with its own name ("USB3 Capture" and "Digital Audio
/// Interface (USB3 Capture)", "Game Capture HD60 S+" and "Game Capture HD60
/// S+ Audio"). Picks the sound input that belongs to `videoName`: the one
/// sharing the most distinctive words with it. -1 when none shares any; a
/// webcam's microphone or the PC's line-in is better left alone than guessed.
[[nodiscard]] int pickSoundFor(std::string_view videoName, const std::vector<std::string>& soundNames);

} // namespace veloplay
