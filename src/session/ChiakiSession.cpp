#include "session/ISession.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <atomic>
#include <thread>
#include <utility>

#include <spdlog/spdlog.h>

#include "media/Audio.hpp"
#include "net/Socket.hpp"
#include "session/StreamMeter.hpp"
#include "vp_session.h"

namespace veloplay::session {

std::string_view toString(QuitReason reason) {
    switch (reason) {
        case QuitReason::Stopped: return "stopped";
        case QuitReason::ConsoleShutDown: return "console_shut_down";
        case QuitReason::ConnectionRefused: return "connection_refused";
        case QuitReason::InUse: return "in_use";
        case QuitReason::ConsoleCrashed: return "console_crashed";
        case QuitReason::VersionMismatch: return "version_mismatch";
        case QuitReason::SetupFailed: return "setup_failed";
        case QuitReason::ConnectionLost: return "connection_lost";
        case QuitReason::PsnNoAnswer: return "psn_no_answer";
        case QuitReason::PsnUnreachable: return "psn_unreachable";
        case QuitReason::PsnFailed: return "psn_failed";
        case QuitReason::PsnRegistFailed: return "psn_regist_failed";
    }
    return "unknown";
}

bool isTransient(QuitReason reason) {
    return reason == QuitReason::ConnectionLost || reason == QuitReason::SetupFailed ||
           reason == QuitReason::ConnectionRefused || reason == QuitReason::PsnUnreachable;
}

namespace {

// The console counts lost packets itself and lowers the bitrate when we
// report loss. chiaki-ng caps the reported value at 5 % so that one burst does
// not push the stream to its lowest quality; the same cap is used here.
constexpr double kReportedPacketLossMax = 0.05;

QuitReason fromShim(int reason) {
    switch (reason) {
        case VP_QUIT_STOPPED: return QuitReason::Stopped;
        case VP_QUIT_CONSOLE_SHUT_DOWN: return QuitReason::ConsoleShutDown;
        case VP_QUIT_CONNECTION_REFUSED: return QuitReason::ConnectionRefused;
        case VP_QUIT_IN_USE: return QuitReason::InUse;
        case VP_QUIT_CONSOLE_CRASHED: return QuitReason::ConsoleCrashed;
        case VP_QUIT_VERSION_MISMATCH: return QuitReason::VersionMismatch;
        case VP_QUIT_CONNECTION_LOST: return QuitReason::ConnectionLost;
        case VP_QUIT_PSN_NO_ANSWER: return QuitReason::PsnNoAnswer;
        case VP_QUIT_PSN_UNREACHABLE: return QuitReason::PsnUnreachable;
        case VP_QUIT_PSN_FAILED: return QuitReason::PsnFailed;
        case VP_QUIT_PSN_REGIST_FAILED: return QuitReason::PsnRegistFailed;
        default: return QuitReason::SetupFailed;
    }
}

int codecFor(const StreamSettings& stream) {
    if (stream.codec == VideoCodec::H264) return VP_CODEC_H264;
    return stream.hdr ? VP_CODEC_HEVC_HDR : VP_CODEC_HEVC;
}

} // namespace

std::optional<ConnectStage> connectStageOf(std::string_view line) {
    // Lines of chiaki-ng (lib/src/session.c, remote/holepunch.c, senkusha.c) that mark a step. A changed
    // wording upstream only costs the screen its detail; the connection does not depend on it.
    struct Mark {
        std::string_view text;
        ConnectStage stage;
    };
    static constexpr Mark kMarks[] = {
        {"websocket_thread_func: connecting", ConnectStage::AtSony},
        {"Calculating packet allocation", ConnectStage::ThroughRouters},
        {"Punching hole", ConnectStage::ThroughRouters},
        {"REGIST - Starting RUDP session", ConnectStage::AtConsole},
        {"Sending session request", ConnectStage::AtConsole},
        {"Ctrl connected", ConnectStage::LoggedIn},
        {"Starting Senkusha", ConnectStage::Measuring},
        {"Takion connecting", ConnectStage::Measuring},
    };
    for (const Mark& mark : kMarks) {
        if (line.find(mark.text) != std::string_view::npos) return mark.stage;
    }
    return std::nullopt;
}

namespace {

class ChiakiSession final : public ISession {
public:
    ChiakiSession() = default;
    ChiakiSession(const ChiakiSession&) = delete;
    ChiakiSession& operator=(const ChiakiSession&) = delete;

    ~ChiakiSession() override {
        if (stopper_.joinable()) stopper_.join();
        if (!handle_) return;
        // Joins the session's threads, so no callback runs after this line.
        if (vp_session_release(handle_) == 0) {
            vp_session_finish(handle_);
            return;
        }
        // Through Sony's servers the goodbye to them can take ten seconds and
        // more; the window must not wait for it.
        std::thread([handle = handle_] { vp_session_finish(handle); }).detach();
    }

    Result<void> start(const SessionConfig& config, IMediaSink* sink) override {
        if (handle_) return Error{ErrorCode::InvalidArgument, "session was already started"};
        if (!config.psn && !net::parseIpv4(config.host)) {
            return Error{ErrorCode::InvalidArgument, "console address is not IPv4"};
        }
        if (config.psn && (config.psn->accessToken.empty() || vp_psn_available() == 0)) {
            return Error{ErrorCode::InvalidArgument, "this build cannot connect through Sony's servers"};
        }
        if (!config.psn && (config.registKey.empty() || config.registKey.size() > sizeof(VpConnectInfo::regist_key))) {
            return Error{ErrorCode::InvalidArgument, "regist key has an unexpected length"};
        }

        static std::once_flag initOnce;
        static bool initOk = false;
        std::call_once(initOnce, [] { initOk = vp_library_init() == 0; });
        if (!initOk) return Error{ErrorCode::Io, "libchiaki could not be initialised"};

        sink_.store(sink);
        psn_ = config.psn.has_value();

        VpConnectInfo info{};
        info.host = config.host.c_str();
        if (config.psn) {
            info.psn_token = config.psn->accessToken.c_str();
            std::memcpy(info.psn_duid, config.psn->consoleId.data(), sizeof(info.psn_duid));
            std::memcpy(info.psn_account_id, config.psn->accountId.data(), sizeof(info.psn_account_id));
            info.psn_pair = config.psn->pair ? 1 : 0;
        }
        std::memcpy(info.regist_key, config.registKey.data(), std::min(config.registKey.size(), sizeof(info.regist_key)));
        std::memcpy(info.rp_key, config.rpKey.data(), sizeof(info.rp_key));
        info.width = static_cast<std::uint32_t>(config.stream.width);
        info.height = static_cast<std::uint32_t>(config.stream.height);
        info.fps = static_cast<std::uint32_t>(config.stream.fps);
        info.bitrate_kbps = static_cast<std::uint32_t>(config.stream.bitrateKbps);
        info.codec = codecFor(config.stream);
        info.dualsense = config.dualSense ? 1 : 0;
        info.packet_loss_max = kReportedPacketLossMax;

        VpCallbacks callbacks{};
        callbacks.user = this;
        callbacks.log = &ChiakiSession::onLog;
        callbacks.connected = &ChiakiSession::onConnected;
        callbacks.login_pin_requested = &ChiakiSession::onLoginPin;
        callbacks.quit = &ChiakiSession::onQuit;
        callbacks.rumble = &ChiakiSession::onRumble;
        callbacks.trigger_effects = &ChiakiSession::onTriggerEffects;
        callbacks.effect_strength = &ChiakiSession::onEffectStrength;
        callbacks.video = &ChiakiSession::onVideo;
        callbacks.audio_format = &ChiakiSession::onAudioFormat;
        callbacks.audio = &ChiakiSession::onAudio;
        callbacks.haptics = &ChiakiSession::onHaptics;
        callbacks.registered = &ChiakiSession::onRegistered;

        handle_ = vp_session_create(&info, &callbacks);
        if (!handle_) return Error{ErrorCode::Io, "session could not be set up"};
        if (vp_session_start(handle_) != 0) {
            vp_session_destroy(handle_);
            handle_ = nullptr;
            return Error{ErrorCode::Io, "session thread could not be started"};
        }
        return {};
    }

    void stop() override {
        if (!handle_) return;
        if (!psn_) {
            vp_session_stop(handle_);
            return;
        }
        // Through Sony's servers stopping takes the library's locks, which a broken link can hold for a long
        // time (seen on the first laptop: the window stood still for minutes). It is asked for on a thread of
        // its own; the destructor waits for that thread, and AppController lets go of such a session elsewhere.
        if (!stopper_.joinable()) stopper_ = std::thread([handle = handle_] { vp_session_stop(handle); });
    }

    bool slowToEnd() const override { return psn_; }
    void detachSink() override { sink_.store(nullptr); }

    std::vector<SessionEvent> pollEvents() override {
        std::vector<SessionEvent> out;
        std::lock_guard lock(mutex_);
        out.swap(events_);
        return out;
    }

    void setControllerState(const input::ControllerState& state, Clock::time_point now) override {
        if (!handle_) return;
        VpControllerState out{};
        out.buttons = state.buttons;
        out.l2 = state.l2;
        out.r2 = state.r2;
        out.left_x = state.leftX;
        out.left_y = state.leftY;
        out.right_x = state.rightX;
        out.right_y = state.rightY;
        for (std::size_t i = 0; i < state.touches.size(); ++i) {
            out.touches[i].active = state.touches[i].active ? 1 : 0;
            out.touches[i].id = state.touches[i].id;
            out.touches[i].x = state.touches[i].x;
            out.touches[i].y = state.touches[i].y;
        }
        out.motion_valid = state.motion.valid ? 1 : 0;
        for (std::size_t i = 0; i < 3; ++i) {
            out.gyro[i] = state.motion.gyro[i];
            out.accel[i] = state.motion.accel[i];
        }
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
        out.motion_time_us = static_cast<std::uint32_t>(micros);
        vp_session_set_controller(handle_, &out);
    }

    Result<void> submitLoginPin(std::string_view digits) override {
        if (!handle_) return Error{ErrorCode::InvalidArgument, "no session"};
        const bool numeric = !digits.empty() && std::all_of(digits.begin(), digits.end(),
                                                             [](char c) { return c >= '0' && c <= '9'; });
        if (!numeric) return Error{ErrorCode::InvalidArgument, "login passcode must be digits"};
        const std::string text(digits);
        if (vp_session_set_login_pin(handle_, text.c_str()) != 0) {
            return Error{ErrorCode::Io, "login passcode could not be handed to the session"};
        }
        return {};
    }

    // The wrapper keeps these three away from a stream that is closing; see vp_session.h.
    bool openMicrophone() override { return handle_ && vp_session_mic_open(handle_) == 0; }
    void muteMicrophone(bool muted) override {
        if (handle_) vp_session_mic_mute(handle_, muted ? 1 : 0);
    }
    void sendMicrophone(std::span<const std::uint8_t> packet) override {
        if (handle_) vp_session_mic_send(handle_, packet.data(), packet.size());
    }

    Result<void> sleepConsole() override {
        if (!handle_) return Error{ErrorCode::InvalidArgument, "no session"};
        if (vp_session_sleep_console(handle_) != 0) {
            return Error{ErrorCode::Network, "rest mode request could not be sent"};
        }
        return {};
    }

    SessionStats stats(Clock::time_point now) override {
        SessionStats out;
        const StreamMeter::Rates rates = meter_.rates(now);
        out.streamFps = rates.framesPerSecond;
        out.bitrateMbps = rates.megabitsPerSecond;
        out.videoFrames = rates.videoFrames;
        out.videoBytes = rates.videoBytes;
        out.audioPackets = rates.audioPackets;
        out.framesLost = rates.framesLost;
        if (handle_) {
            VpLinkStats link{};
            vp_session_link_stats(handle_, &link);
            out.roundTripMs = link.rtt_ms;
            out.packetLossPercent = link.packet_loss * 100.0;
        }
        return out;
    }

private:
    static ChiakiSession& self(void* user) { return *static_cast<ChiakiSession*>(user); }

    void push(SessionEvent event) {
        std::lock_guard lock(mutex_);
        events_.push_back(std::move(event));
    }

    static void onLog(void* user, int level, const char* message) {
        // On a bad link the library reports every missing packet. Writing
        // thousands of lines a second would hold up the very threads that
        // receive the stream, so each second gets a budget.
        ChiakiSession& session = self(user);
        // How far the connection has come, before the budget below may leave the line out.
        if (const auto stage = connectStageOf(message)) {
            const int reached = static_cast<int>(*stage);
            int before = session.stage_.load();
            while (reached > before && !session.stage_.compare_exchange_weak(before, reached)) {}
            if (reached > before) {
                SessionEvent event;
                event.type = SessionEvent::Type::Stage;
                event.stage = *stage;
                session.push(std::move(event));
            }
        }
        {
            std::lock_guard lock(session.logMutex_);
            const auto now = Clock::now();
            if (now - session.logWindow_ >= std::chrono::seconds(1)) {
                if (session.logSuppressed_ > 0) {
                    spdlog::warn("[session] {} more lines in the last second were left out", session.logSuppressed_);
                }
                session.logWindow_ = now;
                session.logLines_ = 0;
                session.logSuppressed_ = 0;
            }
            if (++session.logLines_ > kLogLinesPerSecond) {
                ++session.logSuppressed_;
                return;
            }
        }
        if (level == VP_LOG_ERROR) spdlog::error("[session] {}", message);
        else if (level == VP_LOG_WARNING) spdlog::warn("[session] {}", message);
        else spdlog::info("[session] {}", message);
    }

    static void onConnected(void* user) {
        SessionEvent event;
        event.type = SessionEvent::Type::Connected;
        self(user).push(std::move(event));
    }

    static void onLoginPin(void* user, int previousWasWrong) {
        SessionEvent event;
        event.type = SessionEvent::Type::LoginPinRequested;
        event.previousPinWrong = previousWasWrong != 0;
        self(user).push(std::move(event));
    }

    static void onQuit(void* user, int reason, const char* detail) {
        SessionEvent event;
        event.type = SessionEvent::Type::Quit;
        event.reason = fromShim(reason);
        if (detail) event.detail = detail;
        self(user).push(std::move(event));
    }

    // The same fields a pairing with PIN reads from the console's answer (protocol::parseRegistResponse): the
    // key as the ASCII the console sent in hex, up to the first zero.
    static void onRegistered(void* user, const VpRegisteredHost* host) {
        SessionEvent event;
        event.type = SessionEvent::Type::Registered;
        protocol::RegisteredHost& out = event.registered;
        const auto upToZero = [](const char* text, std::size_t size) { return std::string(text, std::find(text, text + size, '\0')); };
        out.nickname = upToZero(host->nickname, sizeof(host->nickname));
        std::copy(std::begin(host->mac), std::end(host->mac), out.mac.begin());
        out.registKey = upToZero(host->regist_key, sizeof(host->regist_key));
        std::copy(std::begin(host->rp_key), std::end(host->rp_key), out.rpKey.begin());
        out.rpKeyType = host->rp_key_type;
        spdlog::info("The console {} paired with this client through Sony's servers", out.nickname);
        self(user).push(std::move(event));
    }

    static void onRumble(void* user, std::uint8_t low, std::uint8_t high) {
        SessionEvent event;
        event.type = SessionEvent::Type::Rumble;
        event.rumbleLow = low;
        event.rumbleHigh = high;
        self(user).push(std::move(event));
    }

    static void onEffectStrength(void* user, int triggers, int level) {
        SessionEvent event;
        event.type = SessionEvent::Type::EffectStrength;
        event.strengthOfTriggers = triggers != 0;
        switch (level) {
            case 0: event.strength = input::EffectStrength::Off; break;
            case 2: event.strength = input::EffectStrength::Medium; break;
            case 3: event.strength = input::EffectStrength::Weak; break;
            default: event.strength = input::EffectStrength::Strong; break;
        }
        self(user).push(std::move(event));
    }

    static void onTriggerEffects(void* user, std::uint8_t typeLeft, const std::uint8_t* left, std::uint8_t typeRight,
                                 const std::uint8_t* right) {
        SessionEvent event;
        event.type = SessionEvent::Type::TriggerEffects;
        event.triggers.typeLeft = typeLeft;
        event.triggers.typeRight = typeRight;
        std::copy_n(left, kTriggerEffectSize, event.triggers.left.begin());
        std::copy_n(right, kTriggerEffectSize, event.triggers.right.begin());
        self(user).push(std::move(event));
    }

    static int onVideo(void* user, const std::uint8_t* data, std::size_t size, std::int32_t framesLost,
                       int recovered) {
        ChiakiSession& session = self(user);
        session.meter_.addVideo(size, framesLost);
        IMediaSink* sink = session.sink_.load();
        if (!sink) return 1;
        return sink->onVideo({data, size}, framesLost, recovered != 0) ? 1 : 0;
    }

    static void onHaptics(void* user, const std::int16_t* stereo, std::size_t frames) {
        ChiakiSession& session = self(user);
        const std::span<const std::int16_t> samples(stereo, frames * 2);
        if (IMediaSink* sink = session.sink_.load(); sink && sink->onHaptics(samples)) return;

        // Nobody plays the haptics audio, so approximate it with the rumble
        // motors. A packet covers ten milliseconds; three are combined so the
        // controller is not flooded with commands.
        const media::RumbleLevel level = media::hapticsToRumble(samples);
        session.hapticPeak_.low = std::max(session.hapticPeak_.low, level.low);
        session.hapticPeak_.high = std::max(session.hapticPeak_.high, level.high);
        if (++session.hapticPackets_ < kHapticPacketsPerRumble) return;
        session.hapticPackets_ = 0;
        const media::RumbleLevel peak = std::exchange(session.hapticPeak_, {});
        if (peak == session.hapticSent_) return;
        session.hapticSent_ = peak;
        onRumble(user, peak.low, peak.high);
    }

    static void onAudioFormat(void* user, std::uint32_t channels, std::uint32_t bits, std::uint32_t rate,
                              std::uint32_t frameSize) {
        ChiakiSession& session = self(user);
        if (IMediaSink* sink = session.sink_.load()) sink->onAudioFormat(AudioFormat{channels, bits, rate, frameSize});
    }

    static void onAudio(void* user, const std::uint8_t* data, std::size_t size) {
        ChiakiSession& session = self(user);
        session.meter_.addAudio();
        if (IMediaSink* sink = session.sink_.load()) sink->onAudio({data, size});
    }

    static constexpr int kHapticPacketsPerRumble = 3; // as in chiaki-ng
    static constexpr int kLogLinesPerSecond = 40;

    std::mutex logMutex_;
    Clock::time_point logWindow_{};
    int logLines_ = 0;
    int logSuppressed_ = 0;

    VpSession* handle_ = nullptr;
    std::atomic<IMediaSink*> sink_{nullptr};
    std::atomic<int> stage_{0}; // the furthest ConnectStage reached
    bool psn_ = false;     // through Sony's servers
    std::thread stopper_;  // asks a session through Sony's servers to stop, away from the caller
    // Touched by the haptics callback only.
    media::RumbleLevel hapticPeak_{};
    media::RumbleLevel hapticSent_{};
    int hapticPackets_ = 0;
    StreamMeter meter_;
    std::mutex mutex_;
    std::vector<SessionEvent> events_;
};

static_assert(kTriggerEffectSize == VP_TRIGGER_EFFECT_SIZE);

} // namespace

SessionFactory chiakiSessionFactory() {
    return [] { return std::unique_ptr<ISession>(std::make_unique<ChiakiSession>()); };
}

bool psnAvailable() { return vp_psn_available() != 0; }

Result<std::vector<PsnConsole>> listPsnConsoles(const std::string& accessToken) {
    if (!psnAvailable()) return Error{ErrorCode::InvalidArgument, "this build cannot reach Sony's servers"};
    std::array<VpPsnConsole, 16> found{};
    const auto log = [](void*, int level, const char* message) {
        if (level == VP_LOG_ERROR) spdlog::error("[psn] {}", message);
        else spdlog::warn("[psn] {}", message);
    };
    const int count = vp_psn_list_consoles(accessToken.c_str(), found.data(), static_cast<int>(found.size()), log, nullptr);
    if (count < 0) return Error{ErrorCode::Network, "Sony's servers did not list the consoles"};
    std::vector<PsnConsole> out;
    for (int i = 0; i < count; ++i) {
        PsnConsole console;
        console.name = found[static_cast<std::size_t>(i)].name;
        std::copy(std::begin(found[static_cast<std::size_t>(i)].duid), std::end(found[static_cast<std::size_t>(i)].duid),
                  console.id.begin());
        console.remotePlay = found[static_cast<std::size_t>(i)].remote_play_enabled != 0;
        out.push_back(std::move(console));
    }
    return out;
}

} // namespace veloplay::session
