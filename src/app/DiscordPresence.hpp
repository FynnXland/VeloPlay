#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace veloplay::discord {

/// The application VeloPlay registered with Discord. Its name ("VeloPlay")
/// is the head of the card, its pictures are named in the activity.
inline constexpr std::string_view kClientId = "1574032861877502035";

/// What the card shows. Built from the session; nothing else goes in.
struct Activity {
    std::string game;      // empty: no game line (a capture card, or the user does not want it shown)
    std::string state;     // the second line
    std::int64_t since = 0; // Unix seconds when the stream began
    std::string smallImage; // "ps5" or "capture"
    std::string smallText;
    std::string largeText;  // shown when the big picture is pointed at
    std::string buttonLabel;
    std::string buttonUrl;

    friend bool operator==(const Activity&, const Activity&) = default;
};

/// The activity as Discord's SET_ACTIVITY wants it. Texts shorter than two
/// characters are left out: Discord refuses the whole activity for them.
nlohmann::json activityJson(const Activity& activity);

/// One message to or from Discord's local socket: opcode, length, JSON.
enum class Opcode : std::int32_t { Handshake = 0, Frame = 1, Close = 2, Ping = 3, Pong = 4 };
/// The "evt" of a frame from Discord; empty when there is none, it is null, or the frame is no JSON object.
std::string eventOf(const nlohmann::json& body);

std::vector<std::uint8_t> frame(Opcode opcode, const nlohmann::json& body);
struct Message {
    Opcode opcode = Opcode::Frame;
    nlohmann::json body;
};
/// Takes complete messages off the front of `buffer`; a partial one stays.
std::vector<Message> takeMessages(std::vector<std::uint8_t>& buffer);

/// The connection to the Discord app on this PC.
class IPipe {
public:
    virtual ~IPipe() = default;
    /// Opens the connection; false if Discord does not run (or runs only in a browser).
    virtual bool open() = 0;
    virtual void close() = 0;
    virtual bool write(std::span<const std::uint8_t> bytes) = 0;
    /// What has arrived, without waiting. Empty when nothing has; nullopt when the connection broke.
    virtual std::optional<std::vector<std::uint8_t>> read() = 0;
};

/// Keeps the card in Discord in line with what is wanted: connects when
/// Discord is there (and again after Discord restarts), sends a change at
/// most every 15 seconds, as Discord asks of applications, and clears the
/// card when nothing is wanted. Called often; never waits.
class Presence {
public:
    using Clock = std::chrono::steady_clock;
    Presence(std::unique_ptr<IPipe> pipe, std::uint32_t processId);
    ~Presence();

    /// Nothing wanted: the card goes away.
    void update(const std::optional<Activity>& wanted, Clock::time_point now);
    [[nodiscard]] bool connected() const { return ready_; }
    /// What Discord shows right now, as far as this side knows.
    [[nodiscard]] const std::optional<Activity>& shown() const { return shown_; }

private:
    void drop(Clock::time_point now);
    bool send(const std::optional<Activity>& activity);

    std::unique_ptr<IPipe> pipe_;
    std::uint32_t processId_;
    bool open_ = false;
    bool ready_ = false; // Discord answered the handshake
    Clock::time_point retryAt_{};
    Clock::time_point nextSendAt_{};
    std::vector<std::uint8_t> incoming_;
    std::optional<Activity> shown_;
    int nonce_ = 0;
};

/// The pipe to Discord on Windows (\\.\pipe\discord-ipc-0 to -9). Null elsewhere.
std::unique_ptr<IPipe> createPipe();

} // namespace veloplay::discord
