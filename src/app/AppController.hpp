#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "app/AppState.hpp"
#include "app/Config.hpp"
#include "app/DiscordPresence.hpp"
#include "app/Installer.hpp"
#include "app/UiModel.hpp"
#include "input/ControllerManager.hpp"
#include "input/RemoteControl.hpp"
#include "input/KeyboardMouse.hpp"
#include "media/Audio.hpp"
#include "input/VirtualController.hpp"
#include "protocol/ConnectionProbe.hpp"
#include "protocol/DiscoveryService.hpp"
#include "protocol/PsnAccount.hpp"
#include "protocol/Registration.hpp"
#include "session/ISession.hpp"
#include "update/Updater.hpp"

namespace veloplay {

/// Everything the client does that is not drawing: discovery, wake-up, pairing,
/// controllers, the remote control API and the settings file. Platform code
/// (window, renderer, controller backend, secret storage) is passed in.
///
/// All methods must be called from one thread. Background work reports back
/// through update().
class AppController final : public IUiActions {
public:
    struct Options {
        std::filesystem::path configPath;
        ISecretProtector* protector = nullptr; // must outlive the controller
        std::unique_ptr<input::IControllerBackend> controllerBackend;
        std::function<void(std::string_view topic)> openHelp;
        /// Opens an address in the user's own browser.
        std::function<void(const std::string& url)> openAddress;
        /// Starts the program again and ends this instance. Used after an update.
        std::function<void()> restart;
        /// Asks the window for a copy of the picture on screen. Without it there are no screenshots.
        std::function<void()> takeScreenshot;
        /// Where screenshots go, and how the folder is shown.
        std::filesystem::path picturesFolder;
        std::function<void(const std::filesystem::path&)> openFolder;
        /// Capture cards: what is there (asked once per update, so it has to be quick) and how to open one.
        std::function<std::vector<CaptureDevice>()> captureDevices;
        std::function<std::unique_ptr<session::ISession>()> captureFactory;
        /// The connection to Discord for the card "Playing VeloPlay". Without it there is no card.
        std::function<std::unique_ptr<discord::IPipe>()> discordPipe;
        std::uint32_t processId = 0;    // Discord ties the card to this process and clears it when it ends
        std::string systemLanguage;     // for the card's texts when the language setting is "auto"
        /// Starts or ends a recording in the window. Without it there are no videos.
        std::function<void()> toggleRecording;
        /// How the recording stands, asked once per frame.
        std::function<UiModel::RecordingView()> recording;
        std::filesystem::path videosFolder;
        /// Installing on this PC. Without an installer the settings do not offer it.
        IInstaller* installer = nullptr; // must outlive the controller
        /// Starts the installed program and ends this instance.
        std::function<void(const std::filesystem::path& program)> startInstalled;
        /// Self-update needs all three. Leave `http` empty to switch it off.
        update::IHttpClient* http = nullptr; // must outlive the controller
        std::filesystem::path installDir;
        std::string updateApiUrl = update::latestReleaseApiUrl();
        protocol::DiscoveryService::Config discovery{};
        std::uint16_t registPort = protocol::kRegistPort;
        std::chrono::milliseconds wakeTimeout{60'000};
        /// Shows Sony's sign-in page for the account lookup and reports back
        /// through submitAccountRedirect() or accountLoginClosed(). Returns
        /// false if nothing could be shown. Empty switches the lookup off.
        std::function<bool(const std::string& url)> showLogin;
        /// Creates sessions. Empty switches streaming off ("Connect" reports an error).
        session::SessionFactory sessionFactory;
        /// Receives pictures and sound of a session. Null keeps the stream
        /// undecoded (tests, and builds without a renderer). Must outlive the controller.
        session::IMediaSink* mediaSink = nullptr;
        /// A connected session that delivers no picture for this long is
        /// started again at half the bitrate.
        std::chrono::milliseconds stallTimeout{6'000};
        /// How often the controller is read and handed to the console while a
        /// session runs, on a thread of its own. Zero: once per update(), on
        /// the caller's thread. A button press should not have to wait for
        /// the next picture to be drawn.
        std::chrono::microseconds inputInterval{0};
        /// Pause before a failed or lost connection is tried again.
        std::chrono::milliseconds retryDelay{2'000};
        /// After a session of ours ended, the console keeps its place taken
        /// for a while and turns the next one away as "in use". That long it
        /// is tried again, this often.
        std::chrono::milliseconds releaseWait{75'000};
        std::chrono::milliseconds releaseRetry{4'000};
        /// Carries out what the bar at the top of the window asks for. Empty: the window has its own frame.
        std::function<void(WindowAction)> windowAction;
        /// Records the microphone for voice chat. Null: there is none, and
        /// the setting is not offered. Must outlive the controller.
        media::IAudioInput* microphone = nullptr;
        /// Pause before a microphone that could not be opened is tried again.
        std::chrono::milliseconds microphoneRetry{2'000};
    };

    explicit AppController(Options options);
    ~AppController() override;

    /// Loads the settings and starts the background services.
    void start();

    /// Call once per frame.
    void update();

    void fillModel(UiModel& model);

    [[nodiscard]] AppConfig& config() { return config_; }
    [[nodiscard]] AppState state() const { return stateMachine_.state(); }

    /// Physical controller combined with scripted input, ready to be sent to
    /// the console.
    [[nodiscard]] input::ControllerState sessionInput();

    // IUiActions
    void wake(std::string_view hostId) override;
    void connect(std::string_view hostId) override;
    void disconnect() override;
    void sleepAndDisconnect() override;
    void applyStreamSettings() override;
    void captureKey() override;
    void windowAction(WindowAction action) override;
    void testVibration() override;
    void testTriggers(int effect) override;
    void endKeyCapture() override;
    void captureWheel(int target) override;
    void endWheelCapture() override;
    void chooseWheel(const std::string& name) override;

    /// Keys and mouse for playing without a controller. The window passes
    /// on what it gets while the picture has the keyboard; the mapper turns
    /// it into controller input and takes care of the threads itself.
    [[nodiscard]] input::KeyboardMouseMapper& keyboardMouse() { return keyboardMouse_; }
    /// True while the settings wait for a key to give to a control. The
    /// window then hands the next key or mouse button to keyCaptured()
    /// instead of treating it as input; zero stands for "cancelled".
    [[nodiscard]] bool capturingKey() const { return capturingKey_; }
    void keyCaptured(int code);

    /// Pictures the decoder threw away since the stream began, as the media
    /// pipeline counts them. The controller cannot see that itself.
    void noteDroppedFrames(std::uint64_t total) { droppedFrames_ = total; }
    void submitLoginPin(std::string_view digits) override;
    void forgetConsole(std::string_view hostId) override;
    void startPairing(std::string_view address, std::string_view accountId, std::string_view pin) override;
    void cancelPairing() override;
    void dismissPairing() override;
    void dismissNotice(std::size_t index) override;
    void selectProfile(std::string_view profileId) override;
    void configChanged() override;
    void openHelp(std::string_view topic) override;
    void startConnectionTest(std::string_view hostId) override;
    void stopConnectionTest() override;
    void applyStreamAdvice() override;
    void checkForUpdates() override;
    void installUpdate() override;
    void restart() override;
    void installProgram() override;
    void takeScreenshot() override;
    void openPicturesFolder() override;
    void toggleRecording() override;
    void showCapture(const std::string& deviceId) override;
    void openVideosFolder() override;
    /// The window reports how a screenshot went.
    void captureFinished(bool saved, std::string where, std::string detail, bool video = false);
    /// The window made its graphics device anew after it was lost; a running stream says so over the picture.
    void graphicsRestored(std::chrono::steady_clock::time_point now);
    void startAccountLogin() override;
    void startAccountLoginInBrowser() override;
    void submitAccountRedirect(std::string_view url) override;
    void dismissAccountLookup() override;

    /// The sign-in page was closed without a result.
    void accountLoginClosed();

private:
    using Clock = std::chrono::steady_clock;

    struct WakeJob {
        std::string hostId;
        std::uint32_t address = 0;
        std::uint64_t credential = 0;
        Clock::time_point deadline;
        Clock::time_point nextSend;
    };

    /// One connection to a console, across all attempts to set it up.
    struct ActiveSession {
        std::string hostId;
        std::string name;
        std::unique_ptr<session::ISession> session; // empty between attempts
        bool afterWake = false;      // console was just woken and may still be starting
        bool connected = false;      // the current attempt reached the stream
        bool everConnected = false;
        bool userStopped = false;
        bool loginPinRequested = false;
        bool loginPinWrong = false;
        int attempt = 1;
        int attempts = 1;
        std::optional<Clock::time_point> retryAt;
        Clock::time_point connectedAt{};
        int bitrateKbps = 0;           // 0 = what the settings say
        std::uint64_t videoFrames = 0; // as last seen, to notice a stream that stands still
        Clock::time_point lastPicture{};
        bool stalled = false;          // we stopped it because no picture came
        int stalls = 0;                // since the last two minutes without one
        Clock::time_point lastStall{};
        bool markLowered = false;      // automatic lowered its mark for this console once already
        bool restart = false;          // we stopped it to connect again with new settings
        StreamSettings requested{};    // what the running attempt asked the console for
        StreamAlerts alerts;           // what is wrong with the stream right now
        bool capture = false;          // a capture card, not a console: no input, no reconnecting, nothing to learn
        bool waitingForRelease = false; // the console still holds the place of our session before
        Clock::time_point releaseDeadline{}; // until when that is waited for; zero = not waiting
    };

    void addNotice(Notice::Kind kind, Notice::Topic topic, std::string detail, std::string technical = {});
    void beginSession(std::string_view hostId, bool afterWake);
    bool startAttempt();
    [[nodiscard]] StreamSettings streamSettingsForAttempt() const;
    void updateSession(Clock::time_point now);
    void handleQuit(session::QuitReason reason, const std::string& detail, Clock::time_point now);
    void failSession(session::QuitReason reason, const std::string& detail);
    /// Before a session object goes: what it showed about the link, for "automatic".
    void rememberLink(Clock::time_point now);
    void updateAlerts(const session::SessionStats& stats, Clock::time_point now);
    void handleQuickAction(input::QuickAction action);
    [[nodiscard]] std::optional<std::string> addressOf(const RegisteredConsole& console) const;
    void applySettings();
    void updateSessionGame();
    /// Keeps Discord's card in line with the stream and the settings.
    void updatePresence(Clock::time_point now);
    /// The wheel's spring while streaming, and the assignment of its axes and buttons.
    void updateWheel();
    /// Starts or stops the microphone so that it runs exactly while a stream is shown and the setting is on.
    void updateMicrophone(Clock::time_point now);
    /// Ends recording. After it returns nothing reaches the session from the microphone.
    void stopMicrophone();
    void applyRemoteApi();
    void updateDirectTargets();
    void updateWake(Clock::time_point now);
    void updatePairing();
    void updateAccountLookup();
    void updateControllers();
    void updateRemoteStatus(Clock::time_point now);
    void saveNow();

    Options options_;
    std::unique_ptr<ConfigStore> store_;
    AppConfig config_;
    AppStateMachine stateMachine_;

    protocol::DiscoveryService discovery_;
    bool discoveryRunning_ = false;
    std::vector<protocol::DiscoveredHost> discovered_;

    input::ControllerManager controllers_;
    input::VirtualController virtualController_;
    input::KeyboardMouseMapper keyboardMouse_; // keys and mouse as a controller, fed by the window
    std::string sessionGameId_;                // title id of the game the session's console runs; picks the key layout
    std::unique_ptr<media::MicEncoder> micEncoder_; // lives while the microphone records
    bool micLive_ = false;                     // recording and sending to the session
    Clock::time_point micRetryAt_{};           // not tried again before this
    bool micFailureLogged_ = false;
    std::string sessionGameName_;
    input::WheelCapture wheelCapture_;
    std::unique_ptr<discord::Presence> presence_;
    bool presenceFailed_ = false;      // the card threw once; no more tries until VeloPlay starts again
    bool sessionGameLogged_ = false;
    std::string presenceKey_;          // what the card's time belongs to: the source and its game
    std::int64_t presenceSince_ = 0;   // Unix seconds when that began
    bool capturingKey_ = false;                // the settings wait for the key that is pressed next
    int capturedKey_ = 0;
    input::RemoteCommandProcessor remoteProcessor_;
    input::RemoteControlServer remoteServer_;
    RemoteApiSettings appliedRemoteApi_{};
    bool remoteApiApplied_ = false;

    protocol::ConnectionProbe probe_;
    std::string probeHostId_;
    std::string probeName_;
    std::unique_ptr<update::Updater> updater_;
    // Copying the program takes a moment; it runs beside the window and update() picks the outcome up.
    std::future<Result<std::filesystem::path>> installing_;
    std::string installError_;
    UiModel::CaptureNote captureNote_;
    void updateInstall();

    std::unique_ptr<protocol::PsnAccountLookup> accountLookup_;
    AccountLookup accountStatus_;

    std::optional<WakeJob> wake_;
    std::optional<ActiveSession> session_;
    std::atomic<bool> sessionActive_{false}; // read by the remote control thread
    std::vector<Notice> notices_;

    // Pairing runs on its own thread; the result is handed over under the mutex.
    std::jthread pairingThread_;
    std::mutex pairingMutex_;
    std::optional<Result<protocol::RegisteredHost>> pairingResult_;
    std::string pairingAddress_;
    PairingStatus pairing_;

    // Requests from the remote control thread, handled in update().
    std::mutex remoteMutex_;

    // The input pump: reads the controller every few milliseconds and passes
    // it on. pumpSession_ is the session it may talk to; it is cleared under
    // the mutex before that session goes away.
    void pumpInput(std::stop_token stop);
    void attachPump(session::ISession* session);
    std::mutex pumpMutex_;
    session::ISession* pumpSession_ = nullptr;
    std::uint64_t droppedFrames_ = 0; // see noteDroppedFrames()
    std::string lastEndedHostId_;     // the console our last session ran with, and when it ended
    Clock::time_point lastEndedAt_{};
    std::jthread pump_; // last member: stops first
    std::vector<input::QuickAction> pendingActions_;
    std::string remoteStatusJson_ = "{}";
    Clock::time_point nextRemoteStatus_{};

    bool configDirty_ = false;
    Clock::time_point saveAfter_{};
};

} // namespace veloplay
