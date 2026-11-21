#include "session/ISession.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
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
    }
    return "unknown";
}

bool isTransient(QuitReason reason) {
    return reason == QuitReason::ConnectionLost || reason == QuitReason::SetupFailed ||
           reason == QuitReason::ConnectionRefused;
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
        default: return QuitReason::SetupFailed;
    }
}

int codecFor(const StreamSettings& stream) {
    if (stream.codec == VideoCodec::H264) return VP_CODEC_H264;
    return stream.hdr ? VP_CODEC_HEVC_HDR : VP_CODEC_HEVC;
}

class ChiakiSession final : public ISession {
public:
    ChiakiSession() = default;
    ChiakiSession(const ChiakiSession&) = delete;
    ChiakiSession& operator=(const ChiakiSession&) = delete;

    ~ChiakiSession() override {
        // Joins the session thread, so no callback runs after this line.
        if (handle_) vp_session_destroy(handle_);
    }

    Result<void> start(const SessionConfig& config, IMediaSink* sink) override {
        if (handle_) return Error{ErrorCode::InvalidArgument, "session was already started"};
        if (!net::parseIpv4(config.host)) return Error{ErrorCode::InvalidArgument, "console address is not IPv4"};
        if (config.registKey.empty() || config.registKey.size() > sizeof(VpConnectInfo::regist_key)) {
            return Error{ErrorCode::InvalidArgument, "regist key has an unexpected length"};
        }

        static std::once_flag initOnce;
        static bool initOk = false;
        std::call_once(initOnce, [] { initOk = vp_library_init() == 0; });
        if (!initOk) return Error{ErrorCode::Io, "libchiaki could not be initialised"};

        sink_ = sink;

        VpConnectInfo info{};
        info.host = config.host.c_str();
        std::memcpy(info.regist_key, config.registKey.data(), config.registKey.size());
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
        callbacks.video = &ChiakiSession::onVideo;
        callbacks.audio_format = &ChiakiSession::onAudioFormat;
        callbacks.audio = &ChiakiSession::onAudio;
        callbacks.haptics = &ChiakiSession::onHaptics;

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
        if (handle_) vp_session_stop(handle_);
    }

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

    static void onRumble(void* user, std::uint8_t low, std::uint8_t high) {
        SessionEvent event;
        event.type = SessionEvent::Type::Rumble;
        event.rumbleLow = low;
        event.rumbleHigh = high;
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
        if (!session.sink_) return 1;
        return session.sink_->onVideo({data, size}, framesLost, recovered != 0) ? 1 : 0;
    }

    static void onHaptics(void* user, const std::int16_t* stereo, std::size_t frames) {
        ChiakiSession& session = self(user);
        const std::span<const std::int16_t> samples(stereo, frames * 2);
        if (session.sink_ && session.sink_->onHaptics(samples)) return;

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
        if (session.sink_) session.sink_->onAudioFormat(AudioFormat{channels, bits, rate, frameSize});
    }

    static void onAudio(void* user, const std::uint8_t* data, std::size_t size) {
        ChiakiSession& session = self(user);
        session.meter_.addAudio();
        if (session.sink_) session.sink_->onAudio({data, size});
    }

    static constexpr int kHapticPacketsPerRumble = 3; // as in chiaki-ng
    static constexpr int kLogLinesPerSecond = 40;

    std::mutex logMutex_;
    Clock::time_point logWindow_{};
    int logLines_ = 0;
    int logSuppressed_ = 0;

    VpSession* handle_ = nullptr;
    IMediaSink* sink_ = nullptr;
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

} // namespace veloplay::session
