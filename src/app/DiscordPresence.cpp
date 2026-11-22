#include "app/DiscordPresence.hpp"

#include <cstring>

#include <spdlog/spdlog.h>

namespace veloplay::discord {

using namespace std::chrono_literals;

namespace {

// Discord drops what comes faster than this; changes wait instead.
constexpr auto kSendInterval = 15s;
constexpr auto kRetryInterval = 5s;

/// At most `limit` bytes, cut between characters: a cut inside an umlaut would leave a broken one.
std::string shortened(const std::string& text, std::size_t limit) {
    if (text.size() <= limit) return text;
    std::size_t end = limit;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return text.substr(0, end);
}

void putText(nlohmann::json& into, const char* key, const std::string& text) {
    if (text.size() >= 2) into[key] = shortened(text, 128);
}

/// Never throws: broken UTF-8 (a game name as the console sent it) comes out replaced.
std::string dumped(const nlohmann::json& body) {
    return body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

void putInt32(std::vector<std::uint8_t>& out, std::int32_t value) {
    const auto v = static_cast<std::uint32_t>(value);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>((v >> (8 * i)) & 0xFF));
}

std::int32_t getInt32(const std::uint8_t* p) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
                                     (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24));
}

} // namespace

nlohmann::json activityJson(const Activity& activity) {
    nlohmann::json out = nlohmann::json::object();
    putText(out, "details", activity.game);
    putText(out, "state", activity.state);
    // The line in the list of members names the game ("Playing Astro Bot") instead of VeloPlay.
    // Field and values from Discord's activity object: 2 = details.
    if (out.contains("details")) out["status_display_type"] = 2;
    if (activity.since > 0) out["timestamps"] = {{"start", activity.since}};
    nlohmann::json assets = nlohmann::json::object();
    if (activity.platformImage.empty()) {
        assets["large_image"] = "veloplay";
        putText(assets, "large_text", activity.appText);
    } else {
        assets["large_image"] = activity.platformImage;
        putText(assets, "large_text", activity.platformText);
        assets["small_image"] = "veloplay";
        putText(assets, "small_text", activity.appText);
    }
    out["assets"] = assets;
    if (activity.buttonLabel.size() >= 2 && activity.buttonUrl.rfind("https://", 0) == 0) {
        out["buttons"] = nlohmann::json::array(
            {{{"label", shortened(activity.buttonLabel, 32)}, {"url", activity.buttonUrl}}});
    }
    return out;
}

std::string eventOf(const nlohmann::json& body) {
    // Discord answers a command with "evt": null, and a frame that is no JSON at all comes out
    // as a discarded value. value() would throw on both.
    if (!body.is_object()) return {};
    const auto evt = body.find("evt");
    if (evt == body.end() || !evt->is_string()) return {};
    return evt->get<std::string>();
}

std::vector<std::uint8_t> frame(Opcode opcode, const nlohmann::json& body) {
    const std::string text = dumped(body);
    std::vector<std::uint8_t> out;
    out.reserve(8 + text.size());
    putInt32(out, static_cast<std::int32_t>(opcode));
    putInt32(out, static_cast<std::int32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

std::vector<Message> takeMessages(std::vector<std::uint8_t>& buffer) {
    std::vector<Message> out;
    std::size_t at = 0;
    while (buffer.size() - at >= 8) {
        const std::int32_t opcode = getInt32(buffer.data() + at);
        const std::int32_t length = getInt32(buffer.data() + at + 4);
        if (length < 0 || length > 1'000'000) {
            // Nonsense: whatever follows cannot be read either.
            buffer.clear();
            return out;
        }
        if (buffer.size() - at - 8 < static_cast<std::size_t>(length)) break;
        const auto* text = reinterpret_cast<const char*>(buffer.data() + at + 8);
        Message message;
        message.opcode = static_cast<Opcode>(opcode);
        message.body = nlohmann::json::parse(text, text + length, nullptr, false);
        out.push_back(std::move(message));
        at += 8 + static_cast<std::size_t>(length);
    }
    buffer.erase(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(at));
    return out;
}

Presence::Presence(std::unique_ptr<IPipe> pipe, std::uint32_t processId)
    : pipe_(std::move(pipe)), processId_(processId) {}

Presence::~Presence() {
    // The card goes with VeloPlay; Discord would also clear it once the process is gone.
    // So a failure here is no reason to end the program.
    try {
        if (ready_ && shown_) send(std::nullopt);
        if (pipe_ && open_) pipe_->close();
    } catch (const std::exception& error) {
        spdlog::warn("Discord: could not clear the card: {}", error.what());
    }
}

void Presence::drop(Clock::time_point now) {
    if (pipe_ && open_) pipe_->close();
    open_ = false;
    ready_ = false;
    incoming_.clear();
    shown_.reset();
    retryAt_ = now + kRetryInterval;
}

bool Presence::send(const std::optional<Activity>& activity) {
    nlohmann::json args = {{"pid", processId_}};
    if (activity) args["activity"] = activityJson(*activity);
    const nlohmann::json command = {{"cmd", "SET_ACTIVITY"}, {"args", args}, {"nonce", std::to_string(++nonce_)}};
    return pipe_->write(frame(Opcode::Frame, command));
}

void Presence::update(const std::optional<Activity>& wanted, Clock::time_point now) {
    if (!pipe_) return;
    if (!open_) {
        // Nothing to show: no reason to reach for Discord at all.
        if (!wanted || now < retryAt_) return;
        if (!pipe_->open()) {
            retryAt_ = now + kRetryInterval;
            return;
        }
        open_ = true;
        const nlohmann::json hello = {{"v", 1}, {"client_id", std::string(kClientId)}};
        if (!pipe_->write(frame(Opcode::Handshake, hello))) {
            drop(now);
            return;
        }
    }

    std::optional<std::vector<std::uint8_t>> arrived = pipe_->read();
    if (!arrived) {
        spdlog::info("Discord: the connection closed");
        drop(now);
        return;
    }
    incoming_.insert(incoming_.end(), arrived->begin(), arrived->end());
    for (Message& message : takeMessages(incoming_)) {
        switch (message.opcode) {
            case Opcode::Frame:
            {
                const std::string event = eventOf(message.body);
                if (event == "READY") {
                    ready_ = true;
                    spdlog::info("Discord: connected");
                } else if (event == "ERROR") {
                    spdlog::warn("Discord refused the activity: {}", dumped(message.body));
                }
                break;
            }
            case Opcode::Ping:
                pipe_->write(frame(Opcode::Pong, message.body));
                break;
            case Opcode::Close:
                spdlog::info("Discord: closed by Discord ({})", dumped(message.body));
                drop(now);
                return;
            default:
                break;
        }
    }
    // A change waits for Discord's pace; clearing the card does not: a stream that ended should not linger.
    if (!ready_ || (wanted && now < nextSendAt_)) return;
    if (wanted == shown_) return;
    if (!send(wanted)) {
        drop(now);
        return;
    }
    shown_ = wanted;
    nextSendAt_ = now + kSendInterval;
    if (!wanted) {
        // Nothing more to show: let go of Discord until something is.
        pipe_->close();
        open_ = false;
        ready_ = false;
        incoming_.clear();
        retryAt_ = now;
    }
}

} // namespace veloplay::discord

#ifndef _WIN32
namespace veloplay::discord {
std::unique_ptr<IPipe> createPipe() { return nullptr; }
} // namespace veloplay::discord
#endif
