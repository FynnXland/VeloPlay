#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/StreamProfile.hpp"
#include "input/ControllerState.hpp"
#include "protocol/Crypto.hpp"
#include "util/Result.hpp"

// A Remote Play session as the rest of the client sees it. The implementation
// is libchiaki (see ChiakiSession); nothing outside src/session knows that.

namespace veloplay::session {

using Clock = std::chrono::steady_clock;

struct SessionConfig {
    std::string host;        // IPv4 address of the console
    std::string registKey;   // secret, from pairing
    crypto::Key128 rpKey{};  // secret, from pairing
    StreamSettings stream{};
    /// Ask the console for adaptive trigger and haptics data. Only useful with
    /// a DualSense: the console then stops sending classic rumble.
    bool dualSense = false;
};

enum class QuitReason {
    Stopped,           // we ended the session
    ConsoleShutDown,   // console went to rest mode or was switched off
    ConnectionRefused, // console does not accept Remote Play, usually rest mode
    InUse,             // another Remote Play client is connected
    ConsoleCrashed,    // Remote Play on the console crashed
    VersionMismatch,   // console firmware speaks a version we do not
    SetupFailed,       // handshake failed for another reason
    ConnectionLost,    // stream broke or the console dropped us
};

std::string_view toString(QuitReason reason);

/// True for reasons where trying again can help without the user doing anything.
bool isTransient(QuitReason reason);

inline constexpr std::size_t kTriggerEffectSize = 10;

/// Adaptive trigger settings as the console sends them, in the DualSense's own
/// format: an effect type and ten parameter bytes per trigger.
struct TriggerEffects {
    std::uint8_t typeLeft = 0;
    std::uint8_t typeRight = 0;
    std::array<std::uint8_t, kTriggerEffectSize> left{};
    std::array<std::uint8_t, kTriggerEffectSize> right{};

    friend bool operator==(const TriggerEffects&, const TriggerEffects&) = default;
};

struct SessionEvent {
    enum class Type { Connected, LoginPinRequested, Quit, Rumble, TriggerEffects };

    Type type = Type::Connected;
    bool previousPinWrong = false;            // LoginPinRequested
    QuitReason reason = QuitReason::Stopped;  // Quit
    std::string detail;                       // Quit: technical text for the log
    std::uint8_t rumbleLow = 0;               // Rumble: low-frequency motor, 0..255
    std::uint8_t rumbleHigh = 0;              // Rumble: high-frequency motor
    TriggerEffects triggers{};                // TriggerEffects
};

struct AudioFormat {
    std::uint32_t channels = 0;
    std::uint32_t bitsPerSample = 0;
    std::uint32_t sampleRate = 0;
    std::uint32_t frameSize = 0; // samples per channel in one packet
};

/// Receives the stream. All methods are called on threads of the session and
/// must return quickly.
class IMediaSink {
public:
    virtual ~IMediaSink() = default;

    /// A session is about to start with these settings. Called on the
    /// application's thread while no session thread is running.
    virtual void begin(const StreamSettings& stream) { (void)stream; }
    /// The session is gone; nothing else arrives until the next begin().
    virtual void end() {}

    /// One access unit of the video bitstream (Annex B). The parameter sets
    /// arrive as the first call. `framesLost` counts frames missing before it.
    /// Return false if it could not be decoded; the session then asks the
    /// console for a new key frame.
    virtual bool onVideo(std::span<const std::uint8_t> accessUnit, int framesLost, bool recovered) = 0;
    virtual void onAudioFormat(const AudioFormat& format) = 0;
    /// One Opus packet.
    virtual void onAudio(std::span<const std::uint8_t> packet) = 0;
    /// One packet of DualSense haptics: left/right pairs at 3000 Hz. Return
    /// true if it was played. Otherwise the session turns it into Rumble events.
    virtual bool onHaptics(std::span<const std::int16_t> stereo) {
        (void)stereo;
        return false;
    }
};

/// What the session knows about itself right now.
struct SessionStats {
    double streamFps = 0.0;         // video frames per second arriving
    double bitrateMbps = 0.0;       // video data arriving
    double roundTripMs = 0.0;       // measured once during setup
    double packetLossPercent = 0.0; // as reported to the console
    std::uint64_t videoFrames = 0;
    std::uint64_t videoBytes = 0;
    std::uint64_t audioPackets = 0;
    std::uint64_t framesLost = 0;
};

class ISession {
public:
    virtual ~ISession() = default;

    /// Starts the handshake in the background. `sink` may be null; it must
    /// outlive the session otherwise.
    virtual Result<void> start(const SessionConfig& config, IMediaSink* sink) = 0;

    /// Asks the session to end. A Quit event follows.
    virtual void stop() = 0;

    /// Events since the last call, oldest first. A Quit event is the last one
    /// a session produces.
    virtual std::vector<SessionEvent> pollEvents() = 0;

    virtual void setControllerState(const input::ControllerState& state, Clock::time_point now) = 0;

    /// Answers a LoginPinRequested event with the console's login passcode.
    virtual Result<void> submitLoginPin(std::string_view digits) = 0;

    /// Sends the console to rest mode. The session ends shortly after.
    virtual Result<void> sleepConsole() = 0;

    virtual SessionStats stats(Clock::time_point now) = 0;
};

using SessionFactory = std::function<std::unique_ptr<ISession>()>;

/// Sessions backed by libchiaki.
SessionFactory chiakiSessionFactory();

} // namespace veloplay::session
