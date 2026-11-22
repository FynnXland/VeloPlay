#include <gtest/gtest.h>

#include <deque>

#include "app/DiscordPresence.hpp"

using namespace veloplay;
using namespace std::chrono_literals;

namespace {

/// Discord's side of the pipe, scripted by the test.
struct FakeDiscord {
    bool running = true;
    bool broken = false;
    int opens = 0;
    std::vector<discord::Message> received;
    std::vector<std::uint8_t> toSend;
    std::vector<std::uint8_t> partial;

    void say(discord::Opcode opcode, const nlohmann::json& body) {
        const auto bytes = discord::frame(opcode, body);
        toSend.insert(toSend.end(), bytes.begin(), bytes.end());
    }
    [[nodiscard]] std::vector<nlohmann::json> activities() const {
        std::vector<nlohmann::json> out;
        for (const auto& message : received) {
            if (message.opcode == discord::Opcode::Frame && message.body.value("cmd", "") == "SET_ACTIVITY") {
                out.push_back(message.body["args"]);
            }
        }
        return out;
    }
};

struct FakePipe final : discord::IPipe {
    explicit FakePipe(FakeDiscord& discord) : discord_(discord) {}
    bool open() override {
        if (!discord_.running) return false;
        ++discord_.opens;
        discord_.broken = false;
        return true;
    }
    void close() override {}
    bool write(std::span<const std::uint8_t> bytes) override {
        if (discord_.broken) return false;
        discord_.partial.insert(discord_.partial.end(), bytes.begin(), bytes.end());
        for (auto& message : discord::takeMessages(discord_.partial)) discord_.received.push_back(std::move(message));
        return true;
    }
    std::optional<std::vector<std::uint8_t>> read() override {
        if (discord_.broken) return std::nullopt;
        return std::exchange(discord_.toSend, {});
    }

private:
    FakeDiscord& discord_;
};

discord::Activity sample() {
    discord::Activity activity;
    activity.game = "Astro Bot";
    activity.state = "über VeloPlay";
    activity.since = 1'763'800'000;
    activity.smallImage = "ps5";
    activity.smallText = "PS5";
    activity.largeText = "VeloPlay";
    activity.buttonLabel = "VeloPlay holen";
    activity.buttonUrl = "https://github.com/FynnXland/VeloPlay";
    return activity;
}

} // namespace

TEST(DiscordPresence, BuildsTheActivityDiscordExpects) {
    const nlohmann::json json = discord::activityJson(sample());
    EXPECT_EQ(json["details"], "Astro Bot");
    EXPECT_EQ(json["state"], "über VeloPlay");
    EXPECT_EQ(json["timestamps"]["start"], 1'763'800'000); // seconds, not milliseconds
    EXPECT_EQ(json["assets"]["large_image"], "veloplay");
    EXPECT_EQ(json["assets"]["small_image"], "ps5");
    EXPECT_EQ(json["assets"]["small_text"], "PS5");
    ASSERT_EQ(json["buttons"].size(), 1u);
    EXPECT_EQ(json["buttons"][0]["url"], "https://github.com/FynnXland/VeloPlay");

    // Discord refuses an activity with texts of one character; those are left out instead.
    discord::Activity bare = sample();
    bare.game = "X";
    bare.state.clear();
    bare.buttonUrl = "http://not-secure.example";
    const nlohmann::json without = discord::activityJson(bare);
    EXPECT_FALSE(without.contains("details"));
    EXPECT_FALSE(without.contains("state"));
    EXPECT_FALSE(without.contains("buttons"));
    // Long texts are cut to Discord's 128 characters.
    bare.game = std::string(200, 'a');
    EXPECT_EQ(discord::activityJson(bare)["details"].get<std::string>().size(), 128u);
}

TEST(DiscordPresence, FramesGoThereAndBackAndWaitForTheirEnd) {
    auto bytes = discord::frame(discord::Opcode::Frame, {{"evt", "READY"}});
    ASSERT_GT(bytes.size(), 8u);
    EXPECT_EQ(bytes[0], 1); // opcode, little-endian
    std::vector<std::uint8_t> buffer(bytes.begin(), bytes.begin() + 10);
    EXPECT_TRUE(discord::takeMessages(buffer).empty()); // not complete yet
    buffer.insert(buffer.end(), bytes.begin() + 10, bytes.end());
    const auto messages = discord::takeMessages(buffer);
    ASSERT_EQ(messages.size(), 1u);
    EXPECT_EQ(messages[0].body["evt"], "READY");
    EXPECT_TRUE(buffer.empty());
}

TEST(DiscordPresence, ShowsClearsAndKeepsToDiscordsPace) {
    FakeDiscord discord;
    discord::Presence presence(std::make_unique<FakePipe>(discord), 4242);
    auto now = discord::Presence::Clock::now();

    // Nothing wanted: Discord is left alone.
    presence.update(std::nullopt, now);
    EXPECT_EQ(discord.opens, 0);

    // Wanted: handshake first, the activity only after READY.
    presence.update(sample(), now);
    ASSERT_EQ(discord.opens, 1);
    ASSERT_FALSE(discord.received.empty());
    EXPECT_EQ(discord.received[0].opcode, discord::Opcode::Handshake);
    EXPECT_EQ(discord.received[0].body["client_id"], std::string(discord::kClientId));
    EXPECT_TRUE(discord.activities().empty());
    discord.say(discord::Opcode::Frame, {{"evt", "READY"}, {"data", {{"v", 1}}}});
    presence.update(sample(), now);
    ASSERT_EQ(discord.activities().size(), 1u);
    EXPECT_EQ(discord.activities()[0]["pid"], 4242);
    EXPECT_EQ(discord.activities()[0]["activity"]["details"], "Astro Bot");
    EXPECT_TRUE(presence.connected());

    // The same again sends nothing; a change waits for the 15 seconds to pass.
    presence.update(sample(), now + 1s);
    discord::Activity other = sample();
    other.game = "Ratchet & Clank";
    presence.update(other, now + 5s);
    EXPECT_EQ(discord.activities().size(), 1u);
    presence.update(other, now + 16s);
    ASSERT_EQ(discord.activities().size(), 2u);
    EXPECT_EQ(discord.activities()[1]["activity"]["details"], "Ratchet & Clank");

    // A ping is answered with the same content.
    discord.say(discord::Opcode::Ping, {{"x", 1}});
    presence.update(other, now + 17s);
    EXPECT_EQ(discord.received.back().opcode, discord::Opcode::Pong);

    // The stream ends: the card is cleared (an activity command without an activity).
    presence.update(std::nullopt, now + 40s);
    ASSERT_EQ(discord.activities().size(), 3u);
    EXPECT_FALSE(discord.activities()[2].contains("activity"));
    EXPECT_FALSE(presence.connected());
}

TEST(DiscordPresence, WaitsForDiscordAndComesBackAfterItRestarts) {
    FakeDiscord discord;
    discord.running = false;
    discord::Presence presence(std::make_unique<FakePipe>(discord), 1);
    auto now = discord::Presence::Clock::now();
    presence.update(sample(), now);
    EXPECT_EQ(discord.opens, 0);
    // Tried again every five seconds, not every frame.
    discord.running = true;
    presence.update(sample(), now + 1s);
    EXPECT_EQ(discord.opens, 0);
    presence.update(sample(), now + 6s);
    EXPECT_EQ(discord.opens, 1);
    discord.say(discord::Opcode::Frame, {{"evt", "READY"}});
    presence.update(sample(), now + 6s);
    EXPECT_EQ(discord.activities().size(), 1u);

    // Discord quits: the connection breaks, and the card is set again once it is back.
    discord.broken = true;
    presence.update(sample(), now + 7s);
    EXPECT_FALSE(presence.connected());
    presence.update(sample(), now + 13s);
    EXPECT_EQ(discord.opens, 2);
    discord.say(discord::Opcode::Frame, {{"evt", "READY"}});
    presence.update(sample(), now + 30s);
    EXPECT_EQ(discord.activities().size(), 2u);
}
