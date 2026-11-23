#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <fstream>

#include <nlohmann/json.hpp>

#include "FakeProtector.hpp"
#include "app/AutoStream.hpp"
#include "app/Config.hpp"
#include "app/StreamAlerts.hpp"
#include "net/Socket.hpp"

using namespace veloplay;
using json = nlohmann::json;

namespace {

RegisteredConsole sampleConsole() {
    RegisteredConsole console;
    console.name = "Wohnzimmer";
    console.hostId = "001122AABBCC";
    console.lastAddress = "192.168.1.20";
    console.registKey = "1b3d4f5a";
    console.rpKey = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    console.rpKeyType = 2;
    return console;
}

std::filesystem::path tempFile(const char* name) {
    const auto dir = std::filesystem::temp_directory_path() / "veloplay_tests";
    std::filesystem::create_directories(dir);
    const auto path = dir / name;
    std::filesystem::remove(path);
    return path;
}

} // namespace

TEST(StreamProfiles, BuiltInProfilesMatchTheirPurpose) {
    const StreamProfile* smooth = findProfile(kProfileSmooth30);
    ASSERT_NE(smooth, nullptr);
    EXPECT_EQ(smooth->stream.fps, 30);
    EXPECT_EQ(smooth->stream.width, 1920);
    EXPECT_EQ(smooth->stream.codec, VideoCodec::Hevc);
    EXPECT_EQ(smooth->stream.bitrateKbps, 15'000); // what the official client uses for 1080p
    EXPECT_EQ(smooth->frameGenerationFactor, 0); // automatic: doubles a 30 fps stream

    const StreamProfile* native = findProfile(kProfileNative60);
    ASSERT_NE(native, nullptr);
    EXPECT_EQ(native->stream.fps, 60);
    EXPECT_EQ(native->frameGenerationFactor, 0); // automatic: leaves a 60 fps stream alone

    EXPECT_EQ(findProfile("nope"), nullptr);
}

TEST(StreamProfiles, SanitizeClampsToWhatTheConsoleOffers) {
    StreamSettings settings;
    settings.width = 2560;
    settings.height = 1440;
    settings.fps = 48;
    settings.bitrateKbps = 500'000;
    settings.codec = VideoCodec::H264;
    settings.hdr = true;
    sanitize(settings);
    EXPECT_EQ(settings.width, 1920);
    EXPECT_EQ(settings.height, 1080);
    EXPECT_EQ(settings.fps, 60);
    EXPECT_EQ(settings.bitrateKbps, kMaxBitrateKbps);
    EXPECT_FALSE(settings.hdr); // HDR needs HEVC

    settings.bitrateKbps = 1;
    settings.fps = 24;
    sanitize(settings);
    EXPECT_EQ(settings.bitrateKbps, kMinBitrateKbps);
    EXPECT_EQ(settings.fps, 30);
}

TEST(Config, DefaultsAreAutomatic) {
    AppConfig config;
    sanitize(config);
    EXPECT_EQ(config.activeProfile, kProfileAuto);
    EXPECT_TRUE(isAutomatic(config));
    // What is stored underneath is what switching "automatic" off brings back.
    EXPECT_EQ(config.stream.fps, 30);
    // An unknown console gets the choice for a link nothing is known about.
    const StreamSettings asked = effectiveStream(config, "001122334455");
    EXPECT_EQ(asked.fps, 60);
    EXPECT_EQ(asked.height, 1080);
    EXPECT_EQ(asked.codec, VideoCodec::Hevc);
    EXPECT_EQ(asked.bitrateKbps, kAutoStartBitrateKbps);

    // Switched off, the stored values apply and name their profile.
    config.activeProfile = std::string(kProfileCustom);
    sanitize(config);
    EXPECT_EQ(config.activeProfile, kProfileSmooth30);
    EXPECT_EQ(effectiveStream(config, "001122334455"), config.stream);
    EXPECT_EQ(config.graphics.frameGenerationFactor, 0); // automatic
    EXPECT_EQ(config.graphics.superResolutionLevel, 1);
    EXPECT_FALSE(config.remoteApi.enabled); // the API is opt-in
}

TEST(Config, ApplyAndDetectProfile) {
    AppConfig config;
    ASSERT_TRUE(applyProfile(config, kProfileNative60));
    EXPECT_EQ(config.stream.fps, 60);
    EXPECT_EQ(config.graphics.frameGenerationFactor, 0);
    EXPECT_EQ(config.activeProfile, kProfileNative60);

    config.stream.bitrateKbps = 25'000; // edited by hand
    detectProfile(config);
    EXPECT_EQ(config.activeProfile, kProfileCustom);

    EXPECT_FALSE(applyProfile(config, "nope"));
}

TEST(Config, RoundTripsEverything) {
    test::FakeProtector protector;
    AppConfig config;
    config.language = "de";
    applyProfile(config, kProfileNative60);
    config.stream.bitrateKbps = 33'000;
    config.stream.hdr = true;
    config.graphics.superResolutionLevel = 3;
    config.graphics.sharpness = 0.5f;
    config.graphics.deband = false;
    config.graphics.look = 2;
    config.graphics.lookStrength = 0.6f;
    config.graphics.amdFrc = true;
    config.discord.enabled = false;
    config.discord.showGame = false;
    config.controller.wheel.enabled = true;
    config.controller.wheel.device = "Racing Wheel";
    config.controller.wheel.steerAxis = 0;
    config.controller.wheel.gas = {2, 32767, -32768};
    config.controller.wheel.buttons[0] = 5;
    config.controller.wheel.centering = 0.5f;
    config.graphics.vsync = false;
    config.controller.deadzone = 0.12f;
    config.controller.rumble = false;
    config.controller.preferredName = "DualSense";
    config.remoteApi = {true, 9999, "token"};
    config.network = {JitterMode::Stable, false};
    config.updates.checkAtStartup = false;
    config.accountId = "ASNFZ4mrze8=";
    config.manualHosts = {"10.0.0.5", "192.168.1.20"};
    config.consoles = {sampleConsole()};
    sanitize(config);

    const auto text = serializeConfig(config, protector);
    ASSERT_TRUE(text);
    const auto loaded = parseConfig(*text, protector);
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(*loaded, config);
    EXPECT_EQ(loaded->activeProfile, kProfileCustom);
}

TEST(Config, SecretsNeverAppearInTheFile) {
    test::FakeProtector protector;
    AppConfig config;
    config.consoles = {sampleConsole()};

    const auto text = serializeConfig(config, protector);
    ASSERT_TRUE(text);
    EXPECT_EQ(text->find("1b3d4f5a"), std::string::npos);
    EXPECT_EQ(text->find("000102030405060708090a0b0c0d0e0f"), std::string::npos);
    EXPECT_NE(text->find("Wohnzimmer"), std::string::npos); // the name is no secret

    const json root = json::parse(*text);
    EXPECT_FALSE(root["consoles"][0].contains("regist_key"));
    EXPECT_TRUE(root["consoles"][0]["credentials"].is_string());
}

TEST(Config, UnreadableCredentialsKeepTheConsoleButFlagIt) {
    test::FakeProtector writer(0x11);
    test::FakeProtector otherUser(0x22);
    AppConfig config;
    config.consoles = {sampleConsole()};
    const auto text = serializeConfig(config, writer);
    ASSERT_TRUE(text);

    const auto loaded = parseConfig(*text, otherUser);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->consoles.size(), 1u);
    EXPECT_EQ(loaded->consoles[0].name, "Wohnzimmer");
    EXPECT_FALSE(loaded->consoles[0].credentialsValid);
    EXPECT_TRUE(loaded->consoles[0].registKey.empty());

    // Saving again must not fail or invent credentials; what was there is written back unchanged.
    const auto again = serializeConfig(*loaded, otherUser);
    ASSERT_TRUE(again);
    EXPECT_EQ(json::parse(*again)["consoles"][0]["credentials"], json::parse(*text)["consoles"][0]["credentials"]);
}

TEST(Config, ToleratesMissingUnknownAndMistypedFields) {
    test::FakeProtector protector;
    const auto loaded = parseConfig(R"({
        "future_option": true,
        "stream": {"fps": "sixty", "bitrate_kbps": 20000},
        "graphics": "none",
        "remote_api": {"port": 80},
        "manual_hosts": ["192.168.1.20", "not an ip", "192.168.1.20"],
        "consoles": [{"name": "no id"}, 7]
    })", protector);
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded->stream.fps, 30);                   // wrong type: default kept
    EXPECT_EQ(loaded->stream.bitrateKbps, 20000);
    EXPECT_EQ(loaded->graphics.frameGenerationFactor, 0); // nothing usable: the default, automatic
    EXPECT_EQ(loaded->remoteApi.port, 9876);             // privileged port refused
    EXPECT_EQ(loaded->manualHosts, std::vector<std::string>{"192.168.1.20"});
    EXPECT_TRUE(loaded->consoles.empty());
}

TEST(Config, RejectsFilesThatAreNotJson) {
    test::FakeProtector protector;
    EXPECT_FALSE(parseConfig("", protector));
    EXPECT_FALSE(parseConfig("[]", protector));
    EXPECT_FALSE(parseConfig("{broken", protector));
}

TEST(Config, SanitizeClampsRanges) {
    AppConfig config;
    config.language = "fr";
    config.graphics.frameGenerationFactor = 9;
    config.graphics.superResolutionLevel = -1;
    config.graphics.sharpness = 3.0f;
    config.controller.deadzone = 0.9f;
    sanitize(config);
    EXPECT_EQ(config.language, "auto");
    EXPECT_EQ(config.graphics.frameGenerationFactor, 4);
    EXPECT_EQ(config.graphics.superResolutionLevel, 0);
    config.graphics.superResolutionLevel = 7;
    sanitize(config);
    EXPECT_EQ(config.graphics.superResolutionLevel, 2); // 2 is RTX Video, the highest there is
    EXPECT_EQ(config.graphics.sharpness, 1.0f);
    EXPECT_EQ(config.controller.deadzone, 0.5f);
    // A picture style that does not exist is no style; the strength stays between none and all.
    config.graphics.look = 5;
    config.graphics.lookStrength = -0.5f;
    sanitize(config);
    EXPECT_EQ(config.graphics.look, 2);
    EXPECT_EQ(config.graphics.lookStrength, 0.0f);
    EXPECT_EQ(AppConfig{}.graphics.look, 0); // off until asked for
}

TEST(ConfigStore, MissingFileYieldsDefaults) {
    test::FakeProtector protector;
    ConfigStore store(tempFile("missing.json"), protector);
    const auto loaded = store.load();
    ASSERT_TRUE(loaded);
    AppConfig defaults;
    sanitize(defaults);
    EXPECT_EQ(*loaded, defaults);
}

TEST(ConfigStore, SavesAndLoadsWithoutLeavingTemporaryFiles) {
    test::FakeProtector protector;
    const auto path = tempFile("roundtrip.json");
    ConfigStore store(path, protector);

    AppConfig config;
    config.consoles = {sampleConsole()};
    config.accountId = "ASNFZ4mrze8=";
    sanitize(config);
    ASSERT_TRUE(store.save(config));
    ASSERT_TRUE(store.save(config)); // overwriting an existing file works too

    EXPECT_TRUE(std::filesystem::exists(path));
    auto temporary = path;
    temporary += ".tmp";
    EXPECT_FALSE(std::filesystem::exists(temporary));

    const auto loaded = store.load();
    ASSERT_TRUE(loaded);
    EXPECT_EQ(*loaded, config);
}

TEST(ConfigStore, ReportsCorruptFile) {
    test::FakeProtector protector;
    const auto path = tempFile("corrupt.json");
    std::filesystem::remove(std::filesystem::path(path) += ".bak");
    std::ofstream(path) << "{ this is not json";
    ConfigStore store(path, protector);
    const auto loaded = store.load();
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().code, ErrorCode::InvalidData);
    // The broken file is kept aside, so the defaults saved next do not replace the user's pairings.
    EXPECT_NE(loaded.error().message.find("kept as corrupt.broken-"), std::string::npos) << loaded.error().message;
    EXPECT_FALSE(std::filesystem::exists(path));
    bool keptAside = false;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(path).parent_path())) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("corrupt.broken-", 0) == 0) {
            keptAside = true;
            std::filesystem::remove(entry.path());
        }
    }
    EXPECT_TRUE(keptAside);
}

// A power cut while saving can leave an empty file. The copy from before the last save is taken then.
TEST(ConfigStore, FallsBackOnTheCopyFromBeforeTheLastSave) {
    test::FakeProtector protector;
    const auto path = tempFile("backup.json");
    ConfigStore store(path, protector);
    AppConfig config;
    config.consoles = {sampleConsole()};
    sanitize(config);
    ASSERT_TRUE(store.save(config));
    ASSERT_TRUE(store.save(config)); // the first file becomes the copy
    std::ofstream(path, std::ios::trunc).flush(); // empty, as after a power cut
    const auto loaded = store.load();
    ASSERT_TRUE(loaded) << loaded.error().message;
    EXPECT_EQ(loaded->consoles.size(), 1u);
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(path).parent_path())) {
        if (entry.path().filename().string().rfind("backup.broken-", 0) == 0) std::filesystem::remove(entry.path());
    }
}

// Keys that cannot be decrypted right now are written back as they were, not dropped.
TEST(Config, KeepsKeysItCannotDecryptForLater) {
    test::FakeProtector protector(0x11);
    AppConfig config;
    config.consoles = {sampleConsole()};
    const auto text = serializeConfig(config, protector);
    ASSERT_TRUE(text);
    test::FakeProtector otherUser(0x22);
    const auto loaded = parseConfig(*text, otherUser);
    ASSERT_TRUE(loaded);
    ASSERT_FALSE(loaded->consoles[0].credentialsValid);
    const auto again = serializeConfig(*loaded, otherUser);
    ASSERT_TRUE(again);
    // Back with the right account, the pairing is still there.
    const auto restored = parseConfig(*again, protector);
    ASSERT_TRUE(restored);
    EXPECT_TRUE(restored->consoles[0].credentialsValid);
    EXPECT_EQ(restored->consoles[0].rpKey, config.consoles[0].rpKey);
}

// Names come from consoles as they sent them. Broken UTF-8 must not stop saving.
TEST(Config, SavesNamesThatAreNoProperText) {
    test::FakeProtector protector;
    AppConfig config;
    config.consoles = {sampleConsole()};
    config.consoles[0].name = std::string("PS5 \xC3\x28");
    const auto text = serializeConfig(config, protector);
    ASSERT_TRUE(text);
    EXPECT_TRUE(parseConfig(*text, protector));
}

TEST(Config, KeepsWhichNotesOverThePictureAreSwitchedOff) {
    test::FakeProtector protector;
    AppConfig config;
    EXPECT_TRUE(config.notes.enabled);
    for (const StreamAlert alert : kAllStreamAlerts) EXPECT_TRUE(alertShown(config.notes, alert));
    config.notes.hidden = {std::string(alertKey(StreamAlert::BatteryLow))};
    const auto text = serializeConfig(config, protector);
    ASSERT_TRUE(text);
    const auto back = parseConfig(*text, protector);
    ASSERT_TRUE(back);
    EXPECT_FALSE(alertShown(back->notes, StreamAlert::BatteryLow));
    EXPECT_TRUE(alertShown(back->notes, StreamAlert::UnsteadyLink));
    NoteSettings off = back->notes;
    off.enabled = false; // all off at once
    for (const StreamAlert alert : kAllStreamAlerts) EXPECT_FALSE(alertShown(off, alert));
    // Every kind has a name of its own in the file.
    std::vector<std::string_view> keys;
    for (const StreamAlert alert : kAllStreamAlerts) keys.push_back(alertKey(alert));
    std::sort(keys.begin(), keys.end());
    EXPECT_EQ(std::adjacent_find(keys.begin(), keys.end()), keys.end());
}

TEST(Config, KeepsTheSignInForPlayingFromAwaySealed) {
    test::FakeProtector protector;
    AppConfig config;
    config.psn.tokens = {"access-secret", "refresh-secret", 1'800'000'000};
    config.psn.accountId = "IyrEtqCRanA=";
    const auto text = serializeConfig(config, protector);
    ASSERT_TRUE(text);
    // Never in plain text in the file.
    EXPECT_EQ(text->find("access-secret"), std::string::npos);
    EXPECT_EQ(text->find("refresh-secret"), std::string::npos);
    const auto back = parseConfig(*text, protector);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->psn, config.psn);
    EXPECT_TRUE(back->psn.signedIn());

    // Signed out: nothing about it in the file.
    const auto empty = serializeConfig(AppConfig{}, protector);
    ASSERT_TRUE(empty);
    EXPECT_EQ(empty->find("\"psn\""), std::string::npos);
}

TEST(RegisteredConsole, IsBuiltFromRegistrationResult) {
    protocol::RegisteredHost host;
    host.nickname = "PS5-042";
    host.mac = {0x00, 0x11, 0x22, 0xaa, 0xbb, 0xcc};
    host.registKey = "1b3d4f5a";
    host.rpKeyType = 2;
    const RegisteredConsole console = RegisteredConsole::from(host, "192.168.1.20");
    EXPECT_EQ(console.name, "PS5-042");
    EXPECT_EQ(console.hostId, "001122AABBCC");
    EXPECT_EQ(console.lastAddress, "192.168.1.20");
    EXPECT_TRUE(console.credentialsValid);
}

TEST(Config, MovesOldDefaultBitratesToTheCarefulOnes) {
    test::FakeProtector protector;
    // A file from 0.4 or earlier: no version, the old default of the 30 fps profile.
    const auto old30 = parseConfig(R"({"active_profile":"smooth30","stream":{"fps":30,"bitrate_kbps":40000}})", protector);
    ASSERT_TRUE(old30.ok());
    EXPECT_EQ(old30->stream.bitrateKbps, 15'000);

    const auto old60 = parseConfig(R"({"active_profile":"native60","stream":{"fps":60,"bitrate_kbps":50000}})", protector);
    ASSERT_TRUE(old60.ok());
    EXPECT_EQ(old60->stream.bitrateKbps, 20'000);

    // A rate the user picked stays, and so does 40 Mbit/s chosen in a current file.
    const auto chosen = parseConfig(R"({"active_profile":"custom","stream":{"bitrate_kbps":25000}})", protector);
    ASSERT_TRUE(chosen.ok());
    EXPECT_EQ(chosen->stream.bitrateKbps, 25'000);
    const auto current = parseConfig(R"({"settings_version":2,"active_profile":"custom","stream":{"bitrate_kbps":40000}})", protector);
    ASSERT_TRUE(current.ok());
    EXPECT_EQ(current->stream.bitrateKbps, 40'000);

    // What is written now carries the version, so it is not converted again.
    AppConfig config;
    config.stream.bitrateKbps = 40'000;
    const auto text = serializeConfig(config, protector);
    ASSERT_TRUE(text.ok());
    const auto again = parseConfig(*text, protector);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again->stream.bitrateKbps, 40'000);
}

TEST(AutoStream, StartsCarefullyAndKeepsWhatItLearned) {
    const StreamSettings first = automaticStream({});
    EXPECT_EQ(first.width, 1920);
    EXPECT_EQ(first.fps, 60);
    EXPECT_FALSE(first.hdr);
    EXPECT_EQ(first.bitrateKbps, 15'000);
    EXPECT_EQ(automaticStream({22'500, 0}).bitrateKbps, 22'500);
    // A link that only carries very little gets the smaller picture.
    const StreamSettings weak = automaticStream({5'000, 9'000});
    EXPECT_EQ(weak.height, 720);
    EXPECT_EQ(weak.bitrateKbps, 5'000);
    // Whatever the file says, never beyond the limits.
    EXPECT_EQ(automaticStream({90'000, 0}).bitrateKbps, kAutoMaxBitrateKbps);
}

TEST(AutoStream, StartsLowerFromAwayAndKeepsBothMemories) {
    EXPECT_EQ(automaticStream(LinkMemory{}, true).bitrateKbps, kAwayStartBitrateKbps);
    EXPECT_EQ(automaticStream(LinkMemory{}, false).bitrateKbps, kAutoStartBitrateKbps);
    EXPECT_EQ(automaticStream(LinkMemory{12'000, 0}, true).bitrateKbps, 12'000); // learned counts
    test::FakeProtector protector;
    AppConfig config;
    config.consoles = {sampleConsole()};
    config.consoles[0].link = {20'000, 0};
    config.consoles[0].awayLink = {6'000, 8'000};
    const auto text = serializeConfig(config, protector);
    ASSERT_TRUE(text);
    const auto back = parseConfig(*text, protector);
    ASSERT_TRUE(back);
    EXPECT_EQ(back->consoles[0].link, config.consoles[0].link);
    EXPECT_EQ(back->consoles[0].awayLink, config.consoles[0].awayLink);
    EXPECT_EQ(effectiveStream(*back, back->consoles[0].hostId, true).bitrateKbps, 6'000);
    EXPECT_EQ(effectiveStream(*back, back->consoles[0].hostId, false).bitrateKbps, 20'000);
}

TEST(Network, TellsHomeFromAWayThroughATunnel) {
    using net::LocalSubnet;
    const std::uint32_t ps5 = *net::parseIpv4("192.168.178.20");
    // At home: the Wi-Fi or cable is in the console's network.
    EXPECT_TRUE(net::isAtHome(ps5, {LocalSubnet{*net::parseIpv4("192.168.178.31"), 24, false}}));
    // At a friend's: her network is another one, the VPN adapter carries the home network.
    const std::vector<LocalSubnet> away = {LocalSubnet{*net::parseIpv4("192.168.2.40"), 24, false},
                                           LocalSubnet{*net::parseIpv4("192.168.178.201"), 24, true}};
    EXPECT_FALSE(net::isAtHome(ps5, away));
    // Nothing that holds it: also away (a forwarded port, say).
    EXPECT_FALSE(net::isAtHome(ps5, {}));
    EXPECT_TRUE(net::isAtHome(*net::parseIpv4("127.0.0.1"), {}));
    EXPECT_TRUE(net::isAtHome(ps5, {LocalSubnet{ps5, 32, false}}));
}

TEST(AutoStream, RaisesAfterACleanSessionAndLowersAfterTrouble) {
    using std::chrono::seconds;
    // Twenty minutes without a lost picture: a step up, at most to the limit.
    LinkMemory memory = learn({}, {15'000, seconds(1200), 72'000, 0, false});
    EXPECT_EQ(memory.bitrateKbps, 19'000);
    EXPECT_EQ(memory.ceilingKbps, 0);
    memory = learn(memory, {19'000, seconds(1200), 72'000, 3, false});
    EXPECT_EQ(memory.bitrateKbps, 24'000);
    memory = learn(memory, {24'000, seconds(1200), 72'000, 0, false});
    EXPECT_EQ(memory.bitrateKbps, 30'000);
    memory = learn(memory, {30'000, seconds(1200), 72'000, 0, false});
    EXPECT_EQ(memory.bitrateKbps, 30'000);

    // One picture in a hundred lost: that was too much. Back, and remember where the trouble began.
    memory = learn(memory, {30'000, seconds(1200), 71'280, 720, false});
    EXPECT_EQ(memory.bitrateKbps, 22'500);
    EXPECT_EQ(memory.ceilingKbps, 27'000);
    // Clean again: up, but not past that mark.
    memory = learn(memory, {22'500, seconds(1200), 72'000, 0, false});
    EXPECT_EQ(memory.bitrateKbps, 27'000);
    // Clean at the mark itself: the mark moves up a tenth, so a bad evening is not remembered for ever.
    memory = learn(memory, {27'000, seconds(1200), 72'000, 0, false});
    EXPECT_EQ(memory.ceilingKbps, 29'500);
    EXPECT_EQ(memory.bitrateKbps, 29'500);
    // Once it reaches the most automatic asks for, the mark is gone.
    memory = learn(memory, {29'500, seconds(1200), 72'000, 0, false});
    EXPECT_EQ(memory.ceilingKbps, 0);
    EXPECT_EQ(memory.bitrateKbps, 30'000);

    // A little loss is neither good nor bad: stay.
    EXPECT_EQ(learn({20'000, 0}, {20'000, seconds(1200), 72'000, 100, false}).bitrateKbps, 20'000);
    // A short session says nothing.
    EXPECT_EQ(learn({20'000, 0}, {20'000, seconds(30), 1'800, 0, false}), (LinkMemory{20'000, 0}));
    EXPECT_EQ(learn({20'000, 0}, {20'000, seconds(30), 1'500, 300, false}), (LinkMemory{20'000, 0}));
    // A stream that stood still does, however short it was.
    const LinkMemory stalled = learn({}, {15'000, seconds(5), 60, 0, true});
    EXPECT_EQ(stalled.bitrateKbps, 7'500);
    EXPECT_EQ(stalled.ceilingKbps, 13'500);
    // Never below what the console accepts.
    EXPECT_EQ(learn({}, {5'000, seconds(5), 60, 0, true}).bitrateKbps, kMinBitrateKbps);
}

// A weak Wi-Fi evening with two stalls used to leave a console at 720p for good: each stall lowered
// the mark again, and nothing raised it. Now the mark drops once per session.
TEST(AutoStream, LowersTheMarkOncePerSessionAndFindsBackUp) {
    using std::chrono::seconds;
    SessionOutcome first{15'000, seconds(5), 60, 0, true};
    LinkMemory memory = learn({}, first);
    EXPECT_EQ(memory.ceilingKbps, 13'500);
    SessionOutcome second{7'500, seconds(5), 60, 0, true};
    second.markLoweredBefore = true;
    memory = learn(memory, second);
    EXPECT_EQ(memory.bitrateKbps, kMinBitrateKbps); // the rate goes down ...
    EXPECT_EQ(memory.ceilingKbps, 13'500);          // ... the mark stays
    // Clean evenings bring it back to full HD and up to the mark, and past it.
    for (int evening = 0; evening < 6; ++evening) memory = learn(memory, {memory.bitrateKbps, seconds(1200), 72'000, 0, false});
    EXPECT_GE(memory.bitrateKbps, 13'500);
    EXPECT_GT(memory.ceilingKbps, 13'500);
    EXPECT_EQ(automaticStream(memory).height, 1080);
}

TEST(Config, KeepsWhatAutomaticLearnedAboutAConsole) {
    test::FakeProtector protector;
    AppConfig config;
    RegisteredConsole console;
    console.name = "PS5";
    console.hostId = "001122334455";
    console.link = {22'500, 27'000};
    config.consoles.push_back(console);
    const auto text = serializeConfig(config, protector);
    ASSERT_TRUE(text.ok());
    const auto again = parseConfig(*text, protector);
    ASSERT_TRUE(again.ok());
    ASSERT_EQ(again->consoles.size(), 1u);
    EXPECT_EQ(again->consoles[0].link, (LinkMemory{22'500, 27'000}));
    EXPECT_EQ(effectiveStream(*again, "001122334455").bitrateKbps, 22'500);
}

TEST(Config, RemembersTheCornerOfTheDisplay) {
    test::FakeProtector protector;
    AppConfig config;
    EXPECT_EQ(config.network.statsCorner, 0); // top left, where it always was
    config.network.statsCorner = 3;
    const auto text = serializeConfig(config, protector);
    ASSERT_TRUE(text.ok());
    const auto again = parseConfig(*text, protector);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again->network.statsCorner, 3);
    const auto odd = parseConfig(R"({"settings_version":5,"network":{"stats_corner":9}})", protector);
    ASSERT_TRUE(odd.ok());
    EXPECT_EQ(odd->network.statsCorner, 3);
}

TEST(Config, MovesKeysNobodyChangedToTheNewStart) {
    test::FakeProtector protector;
    // A file from 0.17 that holds the keys that version started with.
    AppConfig before;
    before.controller.keyboardMouse.enabled = true;
    before.controller.keyboardMouse.keys = input::firstKeyBindings();
    input::GameKeyLayout game;
    game.titleId = "PPSA01325";
    game.keys = input::firstKeyBindings();
    game.keys[static_cast<std::size_t>(input::PadControl::Triangle)] = 'G'; // somebody chose this
    before.controller.keyboardMouse.games.push_back(game);
    auto text = serializeConfig(before, protector);
    ASSERT_TRUE(text.ok());
    const std::string marker = "\"settings_version\": 5";
    const std::size_t at = text->find(marker);
    ASSERT_NE(at, std::string::npos) << *text;
    std::string old = *text;
    old.replace(at, marker.size(), "\"settings_version\": 4");

    const auto moved = parseConfig(old, protector);
    ASSERT_TRUE(moved.ok());
    EXPECT_EQ(moved->controller.keyboardMouse.keys, input::defaultKeyBindings());
    // A layout somebody made stays as it is.
    ASSERT_EQ(moved->controller.keyboardMouse.games.size(), 1u);
    EXPECT_EQ(moved->controller.keyboardMouse.games[0].keys, game.keys);

    // And a file of today that holds the old keys on purpose keeps them.
    const auto kept = parseConfig(*text, protector);
    ASSERT_TRUE(kept.ok());
    EXPECT_EQ(kept->controller.keyboardMouse.keys, input::firstKeyBindings());
}

TEST(Config, KeepsTheLayoutForMouseAndKeyboard) {
    test::FakeProtector protector;
    AppConfig config;
    // Off by default, with a layout ready for when it is switched on.
    EXPECT_FALSE(config.controller.keyboardMouse.enabled);
    EXPECT_EQ(config.controller.keyboardMouse.keys, input::defaultKeyBindings());

    input::KeyboardMouseSettings& kbm = config.controller.keyboardMouse;
    kbm.enabled = true;
    kbm.mouseSensitivity = 1.75f;
    kbm.invertY = true;
    kbm.keys[static_cast<std::size_t>(input::PadControl::Triangle)] = 'G';
    kbm.keys[static_cast<std::size_t>(input::PadControl::DpadRight)] = input::key::WheelUp;
    kbm.keys[static_cast<std::size_t>(input::PadControl::Create)] = input::key::None;
    const auto text = serializeConfig(config, protector);
    ASSERT_TRUE(text.ok());
    const auto again = parseConfig(*text, protector);
    ASSERT_TRUE(again.ok());
    EXPECT_EQ(again->controller.keyboardMouse, kbm);

    // A game's own layout travels through the file as well.
    input::GameKeyLayout game;
    game.titleId = "PPSA01325";
    game.name = "Astro's Playroom";
    game.keys = kbm.keys;
    game.keys[static_cast<std::size_t>(input::PadControl::Cross)] = 'J';
    game.mouseSensitivity = 0.6f;
    game.invertY = false;
    kbm.games.push_back(game);
    const auto withGame = serializeConfig(config, protector);
    ASSERT_TRUE(withGame.ok());
    const auto readBack = parseConfig(*withGame, protector);
    ASSERT_TRUE(readBack.ok());
    ASSERT_EQ(readBack->controller.keyboardMouse.games.size(), 1u);
    EXPECT_EQ(readBack->controller.keyboardMouse.games[0], game);
    EXPECT_EQ(readBack->controller.keyboardMouse, kbm);

    // A file that names only some controls leaves the others at their default,
    // and a key given twice stays with the first control that has it.
    const auto partial = parseConfig(
        R"({"settings_version":4,"keyboard_mouse":{"enabled":true,"keys":{"cross":69,"circle":69,"nonsense":5}}})", protector);
    ASSERT_TRUE(partial.ok());
    const auto& keys = partial->controller.keyboardMouse.keys;
    EXPECT_EQ(keys[static_cast<std::size_t>(input::PadControl::Cross)], 'E');
    EXPECT_EQ(keys[static_cast<std::size_t>(input::PadControl::Circle)], input::key::None);
    EXPECT_EQ(keys[static_cast<std::size_t>(input::PadControl::LeftStickUp)], 'W');
}

TEST(Config, StartsInFullScreenAndRemembersAWindow) {
    test::FakeProtector protector;
    AppConfig config;
    EXPECT_TRUE(config.window.fullscreen);
    // A file from before the setting existed starts in full screen as well.
    const auto old = parseConfig(R"({"settings_version":4})", protector);
    ASSERT_TRUE(old.ok());
    EXPECT_TRUE(old->window.fullscreen);

    config.window.fullscreen = false;
    const auto text = serializeConfig(config, protector);
    ASSERT_TRUE(text.ok());
    const auto again = parseConfig(*text, protector);
    ASSERT_TRUE(again.ok());
    EXPECT_FALSE(again->window.fullscreen);
}

TEST(Config, TheMicrophoneStaysOffUntilAskedFor) {
    test::FakeProtector protector;
    AppConfig config;
    EXPECT_FALSE(config.audio.microphone);
    // A file from before the setting existed leaves it off as well.
    const auto old = parseConfig(R"({"settings_version":4})", protector);
    ASSERT_TRUE(old.ok());
    EXPECT_FALSE(old->audio.microphone);

    config.audio.microphone = true;
    const auto text = serializeConfig(config, protector);
    ASSERT_TRUE(text.ok());
    const auto again = parseConfig(*text, protector);
    ASSERT_TRUE(again.ok());
    EXPECT_TRUE(again->audio.microphone);
}

TEST(Config, OldFilesGetAutomaticFrameGenerationAndUpscaling) {
    test::FakeProtector protector;
    // What the profiles stored before 0.6 did nothing yet. It becomes "automatic", which does the same.
    const auto old30 = parseConfig(
        R"({"settings_version":2,"stream":{"fps":30},"graphics":{"frame_generation_factor":2,"super_resolution_level":0}})",
        protector);
    ASSERT_TRUE(old30.ok());
    EXPECT_EQ(old30->graphics.frameGenerationFactor, 0);
    EXPECT_EQ(old30->graphics.superResolutionLevel, 1);
    // Files from before 0.9 start with "automatic" once; their values stay underneath.
    EXPECT_EQ(old30->activeProfile, kProfileAuto);
    EXPECT_EQ(old30->stream.fps, 30);

    const auto old60 = parseConfig(
        R"({"settings_version":2,"stream":{"fps":60,"bitrate_kbps":20000},"graphics":{"frame_generation_factor":1}})",
        protector);
    ASSERT_TRUE(old60.ok());
    EXPECT_EQ(old60->graphics.frameGenerationFactor, 0);
    EXPECT_EQ(old60->activeProfile, kProfileAuto);
    EXPECT_EQ(old60->stream.bitrateKbps, 20'000);

    // A file from this version keeps a profile chosen by hand.
    const auto chosen = parseConfig(
        R"({"settings_version":4,"active_profile":"native60","stream":{"fps":60,"bitrate_kbps":20000}})", protector);
    ASSERT_TRUE(chosen.ok());
    EXPECT_EQ(chosen->activeProfile, kProfileNative60);

    // A factor nobody gets by picking a profile was set by hand and stays.
    const auto byHand = parseConfig(
        R"({"settings_version":2,"stream":{"fps":30},"graphics":{"frame_generation_factor":4}})", protector);
    ASSERT_TRUE(byHand.ok());
    EXPECT_EQ(byHand->graphics.frameGenerationFactor, 4);

    // Files written now are left alone: switched off stays switched off.
    const auto current = parseConfig(
        R"({"settings_version":3,"stream":{"fps":30},"graphics":{"frame_generation_factor":2,"super_resolution_level":0}})",
        protector);
    ASSERT_TRUE(current.ok());
    EXPECT_EQ(current->graphics.frameGenerationFactor, 2);
    EXPECT_EQ(current->graphics.superResolutionLevel, 0);
}
