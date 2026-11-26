#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/StreamProfile.hpp"
#include "input/ControllerState.hpp"
#include "input/DualSenseEffects.hpp"
#include "media/VideoFrame.hpp"
#include "protocol/Crypto.hpp"
#include "protocol/Registration.hpp"
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
    std::string name;        // how the user knows the source; a capture card finds its sound input by it
    /// Set: reach the console through Sony's servers instead of `host`. The
    /// access token of a sign-in for playing from away, the console's id at
    /// Sony and the account's eight bytes. The pairing keys are not used then:
    /// the console pairs again on this way.
    struct Psn {
        std::string accessToken; // secret
        std::array<std::uint8_t, 32> consoleId{};
        std::array<std::uint8_t, 8> accountId{};
        /// Pair with the console on this way, without a PIN, and end: a Registered event carries the keys,
        /// then the session quits as Stopped. Connecting is a session of its own afterwards.
        bool pair = false;
    };
    std::optional<Psn> psn;
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
    // Only through Sony's servers:
    PsnNoAnswer,       // the console did not answer Sony's servers: off, asleep or without internet
    PsnUnreachable,    // no way through the two networks was found
    PsnFailed,         // Sony's servers refused or another step failed (an expired sign-in, say)
    PsnRegistFailed,   // the console answered but would not let this client in
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

/// How far a connection has come, for the connecting screen. Within one attempt it only goes forward.
enum class ConnectStage {
    Starting,       // on its way out of this PC
    AtSony,         // signing in at Sony's servers, which tell the console
    ThroughRouters, // looking for a way through the routers on both ends
    AtConsole,      // the console answers
    LoggedIn,       // the console let this client in
    Measuring,      // the link is measured before the picture starts
};

/// The step a line of libchiaki's log stands for, if any. The library has no other way to say how far it is.
[[nodiscard]] std::optional<ConnectStage> connectStageOf(std::string_view logLine);

struct SessionEvent {
    enum class Type { Connected, LoginPinRequested, Quit, Rumble, TriggerEffects, EffectStrength, Registered, Stage };

    Type type = Type::Connected;
    bool previousPinWrong = false;            // LoginPinRequested
    QuitReason reason = QuitReason::Stopped;  // Quit
    std::string detail;                       // Quit: technical text for the log
    std::uint8_t rumbleLow = 0;               // Rumble: low-frequency motor, 0..255
    std::uint8_t rumbleHigh = 0;              // Rumble: high-frequency motor
    TriggerEffects triggers{};                // TriggerEffects
    bool strengthOfTriggers = false;          // EffectStrength: for the triggers, else for vibration and haptics
    input::EffectStrength strength = input::EffectStrength::Strong; // EffectStrength
    /// Registered: what the console handed over when it paired through Sony's servers, as a pairing with PIN
    /// gives it. The keys are secret.
    protocol::RegisteredHost registered{};
    ConnectStage stage = ConnectStage::Starting; // Stage
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

    // A source that delivers pictures and sound as they are (a capture card) skips the decoders.
    /// A picture in CPU memory; its `owner` keeps the pixels alive. False if it was not taken.
    virtual bool onPicture(media::VideoFrame picture) {
        (void)picture;
        return false;
    }
    /// Sound as 16-bit samples from here on, in this format.
    virtual void onPcmFormat(int sampleRate, int channels) {
        (void)sampleRate;
        (void)channels;
    }
    virtual void onPcm(std::span<const std::int16_t> interleaved) { (void)interleaved; }
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

    /// Tells the console that a microphone is there. False while no stream
    /// runs. The three microphone calls may come from any thread; outside a
    /// running stream they do nothing.
    virtual bool openMicrophone() { return false; }
    /// The console starts with the microphone muted.
    virtual void muteMicrophone(bool muted) { (void)muted; }
    /// One packet from media::MicEncoder.
    virtual void sendMicrophone(std::span<const std::uint8_t> packet) { (void)packet; }

    /// Starts the handshake in the background. `sink` may be null; it must
    /// outlive the session otherwise.
    virtual Result<void> start(const SessionConfig& config, IMediaSink* sink) = 0;

    /// Asks the session to end. A Quit event follows.
    virtual void stop() = 0;

    /// True if ending the session can take long and must not be waited for on the window's thread: through
    /// Sony's servers, a link that broke can keep the library's threads busy for minutes.
    [[nodiscard]] virtual bool slowToEnd() const { return false; }
    /// From now on nothing reaches the sink. For a session that is let go of on another thread.
    virtual void detachSink() {}

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

/// A console of the signed-in account, as Sony's servers list it.
struct PsnConsole {
    std::string name;
    std::array<std::uint8_t, 32> id{};
    bool remotePlay = false; // Remote Play is switched on at the console
    friend bool operator==(const PsnConsole&, const PsnConsole&) = default;
};

/// Whether this build can reach a console through Sony's servers.
bool psnAvailable();
/// Asks Sony's servers for the account's PS5 consoles. Blocks for seconds:
/// never on the window thread. `accessToken` is secret and never logged.
Result<std::vector<PsnConsole>> listPsnConsoles(const std::string& accessToken);

} // namespace veloplay::session
