#include <fstream>
#include <iterator>
#include <string>

#include "app/HelpLinks.hpp"
#include "update/Release.hpp"
#include <gtest/gtest.h>

#include <set>
#include <string>

#include "ui/Strings.hpp"

using namespace veloplay::ui;

namespace {

std::set<std::string> placeholders(std::string_view text) {
    std::set<std::string> found;
    for (std::size_t i = 0; i + 2 < text.size(); ++i) {
        if (text[i] == '{' && text[i + 2] == '}') found.insert(std::string(text.substr(i, 3)));
    }
    return found;
}

} // namespace

TEST(Strings, BothLanguagesAreComplete) {
    for (Language language : {Language::German, Language::English}) {
        for (const char* text : allTexts(strings(language))) {
            ASSERT_NE(text, nullptr);
            EXPECT_NE(std::string_view(text), "");
        }
    }
}

TEST(Strings, LanguagesUseTheSamePlaceholders) {
    const auto german = allTexts(strings(Language::German));
    const auto english = allTexts(strings(Language::English));
    ASSERT_EQ(german.size(), english.size());
    for (std::size_t i = 0; i < german.size(); ++i) {
        EXPECT_EQ(placeholders(german[i]), placeholders(english[i])) << german[i] << " / " << english[i];
    }
}

TEST(Strings, FormatReplacesPlaceholders) {
    EXPECT_EQ(format("{0} fps become {1} fps", {"30", "60"}), "30 fps become 60 fps");
    EXPECT_EQ(format("{1}{0}{1}", {"a", "b"}), "bab");
    EXPECT_EQ(format("no placeholder", {"x"}), "no placeholder");
    EXPECT_EQ(format("{2} stays", {"a"}), "{2} stays"); // missing argument is left visible
}

TEST(Strings, LanguageFollowsSettingThenSystem) {
    EXPECT_EQ(resolveLanguage("de", "en"), Language::German);
    EXPECT_EQ(resolveLanguage("en", "de"), Language::English);
    EXPECT_EQ(resolveLanguage("auto", "de"), Language::German);
    EXPECT_EQ(resolveLanguage("auto", "de-AT"), Language::German);
    EXPECT_EQ(resolveLanguage("auto", "fr"), Language::English);
}

// ---- help links ----------------------------------------------------------
// The client sends people to pages in the repository. A link that leads
// nowhere is worse than none, so every target has to exist in this checkout.

namespace {

std::string readRepositoryFile(const std::string& relativePath) {
    std::ifstream file(std::string(VELOPLAY_SOURCE_DIR) + "/" + relativePath, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
}

} // namespace

TEST(HelpLinks, PointAtPagesThatExist) {
    // "HEAD" follows the repository's default branch, whatever it is called.
    EXPECT_EQ(veloplay::helpBaseUrl(), "https://github.com/FynnXland/VeloPlay/blob/HEAD/");

    for (const char* topic : {"account-id", "away", "remote-control", "anything-else", ""}) {
        for (bool german : {true, false}) {
            const std::string url = veloplay::helpUrl(topic, german);
            ASSERT_EQ(url.rfind(veloplay::helpBaseUrl(), 0), 0u) << url;
            std::string path = url.substr(veloplay::helpBaseUrl().size());
            std::string anchor;
            if (const auto hash = path.find('#'); hash != std::string::npos) {
                anchor = path.substr(hash + 1);
                path.resize(hash);
            }
            const std::string content = readRepositoryFile(path);
            EXPECT_FALSE(content.empty()) << url << " leads to a file that is not in the repository";
            if (!anchor.empty()) {
                EXPECT_NE(content.find("id=\"" + anchor + "\""), std::string::npos) << url << " has no such anchor";
            }
        }
    }
    EXPECT_NE(veloplay::helpUrl("account-id", true).find("README.de.md#account-id"), std::string::npos);
    EXPECT_NE(veloplay::helpUrl("account-id", false).find("README.md#account-id"), std::string::npos);
}

TEST(HelpLinks, SendReleaseTopicsToTheReleasePage) {
    EXPECT_EQ(veloplay::helpUrl("releases", true), veloplay::update::releasesPageUrl());
    EXPECT_EQ(veloplay::helpUrl("release-notes", false), veloplay::update::releasesPageUrl());
}
