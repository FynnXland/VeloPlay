#include "app/CaptureMatch.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <set>

namespace veloplay {

namespace {

// Words every sound or video device may carry; they say nothing about which card it is.
constexpr std::array<std::string_view, 14> kCommon = {"audio", "video", "device", "digital", "interface", "input",
                                                       "usb", "microphone", "mic", "line", "in", "the", "and", "hd"};

std::set<std::string> words(std::string_view text) {
    std::set<std::string> out;
    std::string word;
    const auto flush = [&] {
        if (word.size() >= 2 && std::find(kCommon.begin(), kCommon.end(), word) == kCommon.end()) out.insert(word);
        word.clear();
    };
    for (const char c : text) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else {
            flush();
        }
    }
    flush();
    return out;
}

} // namespace

int pickSoundFor(std::string_view videoName, const std::vector<std::string>& soundNames) {
    const std::set<std::string> wanted = words(videoName);
    int best = -1;
    std::size_t bestShared = 0;
    for (std::size_t i = 0; i < soundNames.size(); ++i) {
        std::size_t shared = 0;
        for (const std::string& word : words(soundNames[i])) shared += wanted.count(word);
        if (shared > bestShared) {
            bestShared = shared;
            best = static_cast<int>(i);
        }
    }
    return best;
}

} // namespace veloplay
