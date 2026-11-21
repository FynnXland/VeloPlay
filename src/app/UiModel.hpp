#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/AppState.hpp"
#include "app/Config.hpp"
#include "app/ConsoleList.hpp"
#include "app/StreamAlerts.hpp"
#include "app/StreamStats.hpp"
#include "input/ControllerState.hpp"
#include "input/IControllerBackend.hpp"
#include "protocol/LatencyStats.hpp"
#include "update/Updater.hpp"
#include "util/Result.hpp"

// The contract between the application logic and the screens. The UI reads a
// UiModel every frame and reports what the user did through IUiActions. Neither
// side knows how the other is implemented, which is what lets tools/ui_preview
// render every screen from scripted data.

namespace veloplay {

struct PairingStatus {
    enum class Phase { Idle, Running, Succeeded, Failed };
    Phase phase = Phase::Idle;
    std::string consoleName;                   // set on success
    ErrorCode error = ErrorCode::InvalidData;  // set on failure
    std::string errorDetail;                   // technical message for the log line
};

/// Finding out the PSN account ID by signing in at Sony.
struct AccountLookup {
    enum class Phase {
        Idle,
        SigningIn, // the sign-in page is open, waiting for the user
        Fetching,  // signed in, asking Sony for the account ID
        Succeeded,
        Failed,
    };
    Phase phase = Phase::Idle;
    std::string accountId;                    // set on success, base64 notation
    ErrorCode error = ErrorCode::InvalidData; // set on failure
    std::string errorDetail;
};

/// Something the user should know about, shown as a banner until dismissed.
struct Notice {
    enum class Kind { Warning, Error };
    enum class Topic {
        WakeTimedOut,
        WakeFailed,
        ConfigSaveFailed,
        ConfigLoadFailed,
        DiscoveryFailed,
        RemoteApiFailed,
        // A session did not start or ended on its own. `detail` is the console.
        SessionRefused,         // console does not accept Remote Play right now
        SessionInUse,           // another device is connected
        SessionVersionMismatch, // console firmware is newer than this client knows
        SessionConsoleCrashed,
        SessionSetupFailed,
        SessionLost,            // stream broke and reconnecting did not help
        SleepFailed,            // rest mode request could not be sent
        BitrateLowered,         // no picture at the set rate; `detail` is the new rate in Mbit/s
    };
    Kind kind = Kind::Error;
    Topic topic = Topic::WakeTimedOut;
    std::string detail; // console or controller name, or a technical message
    std::string technical; // optional second line for the log-minded
};

/// A running or finished measurement of the link to one console.
struct ConnectionTest {
    bool running = false;
    std::string hostId;
    std::string consoleName;
    protocol::LatencySnapshot stats;
    protocol::LinkQuality quality = protocol::LinkQuality::Unknown;
};

/// The session that is being set up or running. Meaningful while the state is
/// Connecting, Streaming, Reconnecting or Disconnecting.
struct SessionView {
    std::string hostId;
    std::string consoleName;
    /// The console asks for the login passcode of the PSN user.
    bool loginPinRequested = false;
    bool loginPinWrong = false; // the passcode entered before was rejected
    int attempt = 1;            // which try this is, 1-based
    int attempts = 1;           // how many tries there are in total
    /// The console still counts our session before as running and turns the
    /// new one away. It is tried again until the console lets go.
    bool waitingForRelease = false;
    std::uint64_t videoFrames = 0;
    std::uint64_t audioPackets = 0;
    std::uint64_t framesLost = 0;
    double receivedMegabytes = 0.0;
    int connectedSeconds = 0;
    /// The stream settings were changed since this session asked the console
    /// for its stream. They take effect with the next connection.
    bool settingsChanged = false;
};

struct UiModel {
    AppState state = AppState::Idle;
    std::vector<ConsoleEntry> consoles;
    bool discoveryRunning = false;

    /// The graphics card offers NVIDIA's RTX Video Super Resolution as a way to enlarge the picture.
    bool rtxVideoAvailable = false;
    /// What a session asks the console for right now: the stored settings,
    /// or the program's own choice with "automatic".
    StreamSettings requestedStream;
    /// Waiting for a key to give to a control, and the key that came (0 = none
    /// yet; -1 = the user pressed Esc). Codes as in input/KeyboardMouse.hpp.
    bool capturingKey = false;
    int capturedKey = 0;
    /// Refreshes per second of the screen the window is on; 0 = unknown.
    double displayHz = 0.0;
    bool controllerConnected = false;
    input::ControllerInfo controller;
    std::vector<input::ControllerInfo> controllers;
    input::ControllerState controllerState; // drives gamepad navigation

    PairingStatus pairing;
    AccountLookup accountLookup;
    /// True if this build can open Sony's sign-in page and read the answer.
    bool accountLoginAvailable = false;
    std::vector<Notice> notices;

    ConnectionTest connectionTest;

    /// False on builds that cannot update themselves (anything but Windows).
    bool updaterAvailable = false;
    update::UpdateStatus update;

    SessionView session;

    /// What is wrong with the running stream right now, most pressing first. Shown over the picture.
    std::vector<StreamAlert> alerts;
    /// Present while a stream is running; drives the in-stream display.
    std::optional<StreamStats> stream;

    bool remoteApiRunning = false;
    std::string gpuName;
    bool reducedMotion = false;

    /// Edited in place by the settings screens; they call configChanged() after.
    AppConfig* config = nullptr;
};

class IUiActions {
public:
    virtual ~IUiActions() = default;

    virtual void wake(std::string_view hostId) = 0;
    /// Starts a session. A console in rest mode is woken first.
    virtual void connect(std::string_view hostId) = 0;
    /// Ends the session, or gives up connecting.
    virtual void disconnect() = 0;
    /// Puts the console into rest mode and ends the session.
    virtual void sleepAndDisconnect() = 0;
    /// Answers the console's request for the login passcode.
    /// Ends the running session and connects again with the stream settings as they are now.
    virtual void applyStreamSettings() = 0;
    /// The settings want to know which key or mouse button is pressed next.
    /// It arrives in UiModel::capturedKey; endKeyCapture() ends the waiting
    /// and clears it.
    virtual void captureKey() = 0;
    virtual void endKeyCapture() = 0;
    virtual void submitLoginPin(std::string_view digits) = 0;
    virtual void forgetConsole(std::string_view hostId) = 0;

    /// Starts pairing in the background; progress shows up in UiModel::pairing.
    virtual void startPairing(std::string_view address, std::string_view accountId, std::string_view pin) = 0;
    virtual void cancelPairing() = 0;
    /// The user has seen the result; resets UiModel::pairing to Idle.
    virtual void dismissPairing() = 0;

    virtual void dismissNotice(std::size_t index) = 0;
    virtual void selectProfile(std::string_view profileId) = 0;

    /// Settings were edited. Persists them and applies what can change live.
    virtual void configChanged() = 0;

    virtual void openHelp(std::string_view topic) = 0;

    /// Measures the link to a console until stopConnectionTest().
    virtual void startConnectionTest(std::string_view hostId) = 0;
    virtual void stopConnectionTest() = 0;
    /// Takes over the bitrate and buffer mode that suit the measured link.
    virtual void applyStreamAdvice() = 0;

    virtual void checkForUpdates() = 0;
    virtual void installUpdate() = 0;
    /// Closes the program and starts it again, after an update.
    virtual void restart() = 0;

    /// Opens Sony's sign-in page; progress shows up in UiModel::accountLookup.
    virtual void startAccountLogin() = 0;
    /// Hands over the address Sony sent the browser to after signing in.
    virtual void submitAccountRedirect(std::string_view url) = 0;
    /// The user has seen the outcome, or gives up; resets the lookup to Idle.
    virtual void dismissAccountLookup() = 0;
};

} // namespace veloplay
