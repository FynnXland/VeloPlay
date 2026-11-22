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
    /// The picture comes from a capture card, not from a console over the network.
    bool capture = false;
};

/// A video source on this PC, such as a capture card (or a webcam, which Windows lists the same way).
struct CaptureDevice {
    std::string id;   // what Windows calls its symbolic link; opaque
    std::string name; // as Windows names it

    friend bool operator==(const CaptureDevice&, const CaptureDevice&) = default;
};

/// What the bar at the top of the window can ask of the window.
enum class WindowAction { Minimize, ToggleMaximize, ToggleFullscreen, Close };

struct UiModel {
    AppState state = AppState::Idle;
    /// The window has no title bar from Windows; the screens draw their own
    /// bar with the buttons for the window. False where the window keeps its
    /// frame (and in tools that draw into a picture).
    bool ownWindowBar = false;
    bool windowFullscreen = false;
    bool windowMaximized = false;
    /// The time of day as the bar shows it, like "21:47"; empty shows none.
    std::string clock;
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
    /// The game keys and mouse are laid out for right now: the one running on
    /// the console of the session, or, with no session, on the first console
    /// that runs one. Empty if none is known.
    std::string currentGameId;
    std::string currentGameName;
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

    /// Installing the program on this PC (see IInstaller).
    struct InstallView {
        bool available = false; // false on anything but Windows
        bool installed = false; // the running program is the installed one
        bool busy = false;      // files are being copied
        std::string target;     // the folder it is installed into
        std::string error;      // technical message of an attempt that failed
    };
    InstallView install;

    /// The outcome of the last screenshot. `serial` grows with each one, so the note shows once per picture.
    struct CaptureNote {
        int serial = 0;
        bool video = false; // a recording, not a screenshot
        bool saved = false;
        std::string where;  // the folder, as the user knows it
        std::string detail; // technical message when it failed
    };
    CaptureNote captureNote;
    /// Where screenshots go, as text for the settings; empty where there are none.
    std::string picturesFolder;

    /// Capture cards and other video sources on this PC. Empty where there are none or the build has no support.
    std::vector<CaptureDevice> captureDevices;
    /// The card in Discord ("Playing VeloPlay") can be shown on this build.
    bool discordAvailable = false;

    /// Recording the game as a video.
    struct RecordingView {
        bool available = false; // this PC can record (Windows with Media Foundation)
        std::string missing;    // why not, technical
        bool starting = false;  // the file is being opened
        bool running = false;
        double seconds = 0.0;
    };
    RecordingView recording;
    /// Where videos go, as text for the settings.
    std::string videosFolder;

    /// False on builds that cannot update themselves (anything but Windows).
    bool updaterAvailable = false;
    update::UpdateStatus update;

    SessionView session;

    /// What is wrong with the running stream right now, most pressing first. Shown over the picture.
    std::vector<StreamAlert> alerts;
    /// Present while a stream is running; drives the in-stream display.
    std::optional<StreamStats> stream;

    /// A microphone can be recorded on this PC (false on builds without sound input).
    bool microphoneAvailable = false;
    /// The microphone is being sent to the console right now.
    bool microphoneLive = false;
    /// Switched on and wanted, but no microphone is open: none is plugged in, or Windows keeps it from us.
    bool microphoneMissing = false;

    bool remoteApiRunning = false;
    std::string gpuName;
    /// Who made the graphics card. Features of one maker are hidden in the menu in the game on other
    /// cards and shown greyed out in the settings.
    enum class GpuVendor { Other, Nvidia, Amd, Intel };
    GpuVendor gpuVendor = GpuVendor::Other;
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
    /// The bar at the top asks for something to be done with the window.
    virtual void windowAction(WindowAction action) = 0;
    /// Lets the active controller vibrate for a moment, to try it out. Not during a session.
    virtual void testVibration() = 0;
    /// Puts a test effect on both adaptive triggers: 0 releases them, 1
    /// resistance over the whole travel, 2 a point to push through, 3 shaking.
    /// Not during a session; the console decides then.
    virtual void testTriggers(int effect) = 0;
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
    /// Installs the program on this PC and goes on in the installed one.
    virtual void installProgram() = 0;
    /// Saves the picture as it is on screen, without VeloPlay's overlays. Only while a game is shown.
    virtual void takeScreenshot() = 0;
    /// Shows the folder with the screenshots in Explorer.
    virtual void openPicturesFolder() = 0;
    /// Shows the picture of a capture card, with frame generation and everything else the stream has. Beta.
    virtual void showCapture(const std::string& deviceId) = 0;
    /// Starts recording the game as a video, or ends the recording. Only while a game is shown.
    virtual void toggleRecording() = 0;
    /// Shows the folder with the videos in Explorer.
    virtual void openVideosFolder() = 0;

    /// Opens Sony's sign-in page; progress shows up in UiModel::accountLookup.
    virtual void startAccountLogin() = 0;
    /// Hands over the address Sony sent the browser to after signing in.
    virtual void submitAccountRedirect(std::string_view url) = 0;
    /// The user has seen the outcome, or gives up; resets the lookup to Idle.
    virtual void dismissAccountLookup() = 0;
};

} // namespace veloplay
