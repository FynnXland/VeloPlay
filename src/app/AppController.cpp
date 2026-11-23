#include "app/AppController.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "input/InputMerger.hpp"
#include "ui/Strings.hpp"
#include "update/Release.hpp"
#include "util/StallWatch.hpp"
#include "net/Socket.hpp"
#include "protocol/RegistrationClient.hpp"

namespace veloplay {

namespace {

using json = nlohmann::json;
using std::chrono::milliseconds;

constexpr milliseconds kSaveDelay{600};
constexpr milliseconds kWakeResendInterval{5000};
constexpr milliseconds kRemoteStatusInterval{250};

// A console that just woke up answers discovery a few seconds before it
// accepts Remote Play, so the first tries after a wake-up may be refused.
constexpr int kAttemptsAfterWake = 6;

std::string trimmed(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}
// A stream that broke is tried again this often before the user is told.
constexpr int kReconnectAttempts = 3;
// The console sends a rumble event per change, not per frame. chiaki-ng lets a
// strength run for five seconds unless the next event replaces it.
constexpr milliseconds kRumbleDuration{5000};
// How often a stream that stands still is restarted at a lower rate before giving up.
constexpr int kMaxStalls = 3;

Notice::Topic noticeTopicFor(session::QuitReason reason, bool everConnected) {
    switch (reason) {
        case session::QuitReason::InUse: return Notice::Topic::SessionInUse;
        case session::QuitReason::VersionMismatch: return Notice::Topic::SessionVersionMismatch;
        case session::QuitReason::ConsoleCrashed: return Notice::Topic::SessionConsoleCrashed;
        case session::QuitReason::ConnectionRefused:
            return everConnected ? Notice::Topic::SessionLost : Notice::Topic::SessionRefused;
        case session::QuitReason::ConnectionLost: return Notice::Topic::SessionLost;
        case session::QuitReason::PsnNoAnswer: return Notice::Topic::PsnNoAnswer;
        case session::QuitReason::PsnUnreachable: return Notice::Topic::PsnUnreachable;
        case session::QuitReason::PsnFailed:
        case session::QuitReason::PsnRegistFailed: return Notice::Topic::PsnFailed;
        default: return everConnected ? Notice::Topic::SessionLost : Notice::Topic::SessionSetupFailed;
    }
}

std::string_view statusName(ConsoleStatus status) {
    switch (status) {
        case ConsoleStatus::Ready: return "ready";
        case ConsoleStatus::Standby: return "standby";
        case ConsoleStatus::Offline: break;
    }
    return "offline";
}

} // namespace

AppController::AppController(Options options)
    : options_(std::move(options)),
      discovery_(options_.discovery),
      controllers_(std::move(options_.controllerBackend)),
      remoteProcessor_(virtualController_,
                       input::RemoteControlHooks{
                           [this] {
                               std::lock_guard lock(remoteMutex_);
                               return json::parse(remoteStatusJson_, nullptr, false);
                           },
                           [this](input::QuickAction action) -> std::string {
                               switch (action) {
                                   case input::QuickAction::NextProfile: break;
                                   case input::QuickAction::Sleep:
                                   case input::QuickAction::Disconnect:
                                       if (!sessionActive_.load()) return "no_active_session";
                                       break;
                                   default:
                                       // Microphone, screenshot and the frame generation
                                       // switch arrive with picture and sound.
                                       return sessionActive_.load() ? "not_available" : "no_active_session";
                               }
                               std::lock_guard lock(remoteMutex_);
                               pendingActions_.push_back(action);
                               return {};
                           },
                       }),
      remoteServer_(remoteProcessor_) {
    if (options_.protector) store_ = std::make_unique<ConfigStore>(options_.configPath, *options_.protector);
    if (options_.http && !options_.installDir.empty()) {
        update::Updater::Options updaterOptions;
        updaterOptions.current = update::Version::parse(VELOPLAY_VERSION).value_or(update::Version{});
        updaterOptions.installDir = options_.installDir;
        updaterOptions.workDir = options_.configPath.parent_path() / "update";
        updaterOptions.http = options_.http;
        updaterOptions.apiUrl = options_.updateApiUrl;
        updater_ = std::make_unique<update::Updater>(std::move(updaterOptions));
    }
    if (options_.http) accountLookup_ = std::make_unique<protocol::PsnAccountLookup>(*options_.http);
    sanitize(config_);
    stateMachine_.setObserver([](AppState from, AppState to, AppEvent event) {
        spdlog::info("State: {} -> {} ({})", toString(from), toString(to), toString(event));
    });
}

AppController::~AppController() {
    if (installing_.valid()) installing_.wait(); // the copy uses the installer, which outlives only the controller
    // The pump goes first: it reads the controllers and talks to the session.
    if (pump_.joinable()) {
        pump_.request_stop();
        pump_.join();
    }
    if (session_ && session_->session) {
        attachPump(nullptr);
        logLinkSummary(Clock::now());
        session_->session.reset(); // waits for the session thread
        if (options_.mediaSink) options_.mediaSink->end();
        controllers_.rumble(0, 0, milliseconds{0});
        controllers_.clearTriggerEffects();
    }
    attachPump(nullptr);
    session_.reset();
    probe_.stop();
    updater_.reset();
    cancelPairing();
    if (configDirty_) saveNow();
    remoteServer_.stop();
    discovery_.stop();
}

void AppController::start() {
    if (options_.inputInterval.count() > 0 && !pump_.joinable()) {
        pump_ = std::jthread([this](std::stop_token stop) { pumpInput(stop); });
    }
    if (store_) {
        auto loaded = store_->load();
        if (loaded) {
            config_ = std::move(*loaded);
        } else {
            spdlog::error("Loading settings failed: {}", loaded.error().message);
            addNotice(Notice::Kind::Error, Notice::Topic::ConfigLoadFailed, loaded.error().message);
        }
    }

    const auto started = discovery_.start();
    discoveryRunning_ = started.ok();
    if (!started) {
        spdlog::error("Discovery could not start: {}", started.error().message);
        addNotice(Notice::Kind::Error, Notice::Topic::DiscoveryFailed, started.error().message);
    }
    applySettings();

    if (updater_) {
        // Files a previous update renamed out of the way can go now.
        update::removeLeftovers(options_.installDir);
        if (config_.updates.checkAtStartup) updater_->check();
    }
}

void AppController::update() {
    const auto now = Clock::now();
    diag::setStep("reading the console list");
    discovered_ = discovery_.hosts();
    if (firstUpdate_ == Clock::time_point{}) firstUpdate_ = now;
    for (const protocol::DiscoveredHost& host : discovered_) {
        auto seen = std::find_if(seenAtHome_.begin(), seenAtHome_.end(),
                                 [&](const auto& entry) { return equalsIgnoreCase(entry.first, host.info.hostId); });
        if (seen == seenAtHome_.end()) seenAtHome_.emplace_back(host.info.hostId, now);
        else seen->second = now;
    }

    diag::setStep("reading the controllers");
    updateControllers();
    updateSessionGame();
    diag::setStep("wake-up");
    updateWake(now);
    diag::setStep("session");
    updateSession(now);
    updateMicrophone(now);
    // The card in Discord is an extra. Whatever goes wrong with it must never end the game.
    if (!presenceFailed_) {
        try {
            updatePresence(now);
        } catch (const std::exception& error) {
            spdlog::error("Discord: the card is switched off until VeloPlay starts again: {}", error.what());
            presenceFailed_ = true;
            presence_.reset();
        }
    }
    updateWheel();
    diag::setStep("pairing, remote control, settings");
    updatePairing();
    updateAccountLookup();
    updatePsn(now);
    updateInstall();

    std::vector<input::QuickAction> actions;
    {
        std::lock_guard lock(remoteMutex_);
        actions.swap(pendingActions_);
    }
    for (input::QuickAction action : actions) handleQuickAction(action);

    updateRemoteStatus(now);

    if (configDirty_ && now >= saveAfter_) saveNow();
}

void AppController::fillModel(UiModel& model) {
    model.state = stateMachine_.state();
    model.consoles = buildConsoleList(config_.consoles, discovered_);
    for (ConsoleEntry& entry : model.consoles) {
        entry.waking = wake_ && wake_->hostId == entry.hostId;
        if (entry.registered && entry.status == ConsoleStatus::Offline) {
            for (const RegisteredConsole& console : config_.consoles) {
                if (console.hostId == entry.hostId) entry.viaInternet = goesThroughSony(console, Clock::now());
            }
        }
    }
    model.psnRemote.available = static_cast<bool>(options_.psnConsoles) && options_.http && options_.showLogin;
    model.psnRemote.signedIn = config_.psn.signedIn();
    model.psnRemote.phase = psnPhase_;
    model.psnRemote.errorDetail = psnError_;
    model.psnRemote.listed = psnListed_;
    model.psnRemote.expired = psnExpired_;
    model.psnRemote.consoles.clear();
    for (const session::PsnConsole& console : psnConsoles_) {
        if (console.remotePlay) model.psnRemote.consoles.push_back(console.name);
    }
    model.discoveryRunning = discoveryRunning_;

    const auto active = controllers_.active();
    model.controllerConnected = active.has_value();
    model.controller = active.value_or(input::ControllerInfo{});
    model.controllers = controllers_.controllers();
    model.controllerState = controllers_.state();
    model.microphoneAvailable = options_.microphone != nullptr;
    model.microphoneLive = micLive_ && options_.microphone->recording();
    model.microphoneMissing = micLive_ && !options_.microphone->recording();
    model.currentGameId = sessionGameId_;
    model.currentGameName = sessionGameName_;
    if (!session_) {
        for (const ConsoleEntry& entry : model.consoles) {
            if (entry.status != ConsoleStatus::Ready || entry.runningAppId.empty()) continue;
            model.currentGameId = entry.runningAppId;
            model.currentGameName = entry.runningApp;
            break;
        }
    }

    model.pairing = pairing_;
    model.accountLookup = accountStatus_;
    model.accountLoginAvailable = accountLookup_ != nullptr && static_cast<bool>(options_.showLogin);
    model.notices = notices_;
    model.connectionTest.running = probe_.running();
    model.connectionTest.hostId = probeHostId_;
    model.connectionTest.consoleName = probeName_;
    if (probe_.running()) {
        model.connectionTest.stats = probe_.snapshot();
        model.connectionTest.quality = protocol::rateLink(model.connectionTest.stats);
    }

    model.session = SessionView{};
    model.stream.reset();
    model.alerts.clear();
    if (session_ && session_->connected) model.alerts = session_->alerts.active(Clock::now());
    if (session_) {
        const auto now = Clock::now();
        model.session.hostId = session_->hostId;
        model.session.consoleName = session_->name;
        model.session.loginPinRequested = session_->loginPinRequested;
        model.session.loginPinWrong = session_->loginPinWrong;
        model.session.attempt = session_->attempt;
        model.session.attempts = session_->attempts;
        model.session.waitingForRelease = session_->waitingForRelease;
        model.session.capture = session_->capture;
        model.session.away = session_->away;
        if (session_->session && session_->connected) {
            const session::SessionStats stats = session_->session->stats(now);
            model.session.videoFrames = stats.videoFrames;
            model.session.audioPackets = stats.audioPackets;
            model.session.framesLost = stats.framesLost;
            model.session.receivedMegabytes = static_cast<double>(stats.videoBytes) / 1'000'000.0;
            model.session.connectedSeconds = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(now - session_->connectedAt).count());
            model.session.settingsChanged = !session_->capture && !(session_->requested == streamSettingsForAttempt());

            StreamStats view;
            view.presenting = false; // decoding and display arrive with the next milestone
            view.streamFps = static_cast<float>(stats.streamFps);
            view.bitrateMbps = static_cast<float>(stats.bitrateMbps);
            view.roundTripMs = static_cast<float>(stats.roundTripMs);
            view.packetLossPercent = static_cast<float>(stats.packetLossPercent);
            model.stream = std::move(view);
        }
    }

    if (options_.captureDevices && options_.captureFactory) model.captureDevices = options_.captureDevices();
    model.discordAvailable = static_cast<bool>(options_.discordPipe);
    model.wheel = UiModel::WheelView{};
    model.wheel.devices = controllers_.joysticks();
    model.wheel.capturing = wheelCapture_.active() ? static_cast<int>(wheelCapture_.target()) : -1;
    for (const input::JoystickInfo& device : model.wheel.devices) {
        if (device.name == config_.controller.wheel.device) model.wheel.force = device.force;
    }
    if (const auto raw = controllers_.readWheel()) {
        model.wheel.present = true;
        const input::ControllerState mapped = input::mapWheel(config_.controller.wheel, *raw);
        model.wheel.steer = static_cast<float>(mapped.leftX) / 32767.0f;
        model.wheel.gas = static_cast<float>(mapped.r2) / 255.0f;
        model.wheel.brake = static_cast<float>(mapped.l2) / 255.0f;
        model.wheel.pressed = mapped.buttons;
    }
    model.updaterAvailable = updater_ != nullptr;
    model.install.available = options_.installer != nullptr;
    if (options_.installer) {
        model.install.installed = options_.installer->installed();
        model.install.target = install::utf8(options_.installer->target());
    }
    model.install.busy = installing_.valid();
    model.captureNote = captureNote_;
    if (options_.takeScreenshot) model.picturesFolder = install::utf8(options_.picturesFolder);
    if (options_.recording) model.recording = options_.recording();
    if (options_.toggleRecording) model.videosFolder = install::utf8(options_.videosFolder);
    model.install.error = installError_;
    if (updater_) model.update = updater_->status();

    model.remoteApiRunning = remoteServer_.running();
    model.capturingKey = capturingKey_;
    model.capturedKey = capturedKey_;
    model.config = &config_;
    // What a session asks for: with the console of the running session, or
    // else the first paired one. The settings screen shows it.
    model.requestedStream = effectiveStream(config_, session_ ? std::string_view(session_->hostId)
                                            : config_.consoles.empty() ? std::string_view{}
                                                                       : std::string_view(config_.consoles.front().hostId),
                                            session_ && session_->away);
    model.requestedStream.hdr = false;
}

void AppController::attachPump(session::ISession* session) {
    // Every place that lets go of a session comes through here first, with
    // null. The microphone must be silent before the session goes.
    if (!session) stopMicrophone();
    std::lock_guard lock(pumpMutex_);
    pumpSession_ = session;
}

void AppController::stopMicrophone() {
    if (!micLive_) return;
    micLive_ = false;
    if (options_.microphone) options_.microphone->stop(); // returns once the sink is quiet
    micEncoder_.reset();
    spdlog::info("Microphone off");
}

void AppController::updateMicrophone(Clock::time_point now) {
    media::IAudioInput* microphone = options_.microphone;
    if (!microphone) return;
    const bool streaming = session_ && session_->session && session_->connected && stateMachine_.state() == AppState::Streaming;
    // A capture card has no way back to the console for the microphone.
    const bool wanted = streaming && config_.audio.microphone && !session_->capture;
    if (!wanted) {
        if (micLive_) {
            session::ISession* session = session_ ? session_->session.get() : nullptr;
            stopMicrophone();
            // Still connected: the console is told, so it shows the microphone as muted.
            if (streaming && session) session->muteMicrophone(true);
        }
        micRetryAt_ = {};
        micFailureLogged_ = false;
        return;
    }
    // The device went away while recording (unplugged, or Windows moved to another one): start over.
    if (micLive_ && !microphone->running()) {
        spdlog::warn("The microphone stopped; trying to open it again");
        stopMicrophone();
        micRetryAt_ = now + options_.microphoneRetry;
    }
    if (micLive_ || now < micRetryAt_) return;

    session::ISession* session = session_->session.get();
    auto encoder = std::make_unique<media::MicEncoder>();
    bool ok = encoder->valid() && session->openMicrophone();
    if (ok) {
        micEncoder_ = std::move(encoder);
        // Runs on the recording thread. Both pointers stay valid until stop() has returned.
        ok = microphone->start([session, packer = micEncoder_.get()](std::span<const std::int16_t> block) {
            const std::span<const std::uint8_t> packet = packer->encode(block);
            if (!packet.empty()) session->sendMicrophone(packet);
        });
        if (!ok) micEncoder_.reset();
    }
    if (!ok) {
        if (!micFailureLogged_) spdlog::warn("The microphone could not be started; trying again every {} ms", options_.microphoneRetry.count());
        micFailureLogged_ = true;
        micRetryAt_ = now + options_.microphoneRetry;
        return;
    }
    session->muteMicrophone(false);
    micLive_ = true;
    micFailureLogged_ = false;
    spdlog::info("Microphone on: its sound goes to the console");
}

void AppController::pumpInput(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(options_.inputInterval);
        std::lock_guard lock(pumpMutex_);
        if (!pumpSession_) continue;
        const auto now = Clock::now();
        // Straight from the device, not what update() saw at the last picture.
        // Controller first, then keys and mouse, then scripts: each adds to what is before it.
        pumpSession_->setControllerState(
            input::mergeInputs(input::mergeInputs(input::mergeInputs(controllers_.readNow(), controllers_.wheelNow()),
                                                  keyboardMouse_.state(now)),
                               virtualController_.state(now)),
            now);
    }
}

input::ControllerState AppController::sessionInput() {
    const auto now = Clock::now();
    return input::mergeInputs(
        input::mergeInputs(input::mergeInputs(controllers_.state(), controllers_.wheelNow()), keyboardMouse_.state(now)),
        virtualController_.state(now));
}

// ---- actions -----------------------------------------------------------

void AppController::wake(std::string_view hostId) {
    const auto console = std::find_if(config_.consoles.begin(), config_.consoles.end(),
                                      [&](const RegisteredConsole& c) { return c.hostId == hostId; });
    if (console == config_.consoles.end() || !console->credentialsValid) return;

    const auto credential = protocol::wakeCredential(console->registKey);
    std::optional<std::uint32_t> address = net::parseIpv4(console->lastAddress);
    for (const protocol::DiscoveredHost& host : discovered_) {
        if (equalsIgnoreCase(host.info.hostId, console->hostId)) address = host.address;
    }
    if (!credential || !address) {
        addNotice(Notice::Kind::Error, Notice::Topic::WakeFailed, console->name);
        return;
    }

    if (stateMachine_.state() == AppState::Error) stateMachine_.handle(AppEvent::ErrorAcknowledged);
    if (!stateMachine_.handle(AppEvent::WakeRequested)) return;

    const auto now = Clock::now();
    wake_ = WakeJob{console->hostId, *address, *credential, now + options_.wakeTimeout, now};
    updateWake(now);
}

std::optional<std::string> AppController::addressOf(const RegisteredConsole& console) const {
    for (const protocol::DiscoveredHost& host : discovered_) {
        if (equalsIgnoreCase(host.info.hostId, console.hostId)) return net::formatIpv4(host.address);
    }
    if (net::parseIpv4(console.lastAddress)) return console.lastAddress;
    return std::nullopt;
}

void AppController::connect(std::string_view hostId) {
    const auto console = std::find_if(config_.consoles.begin(), config_.consoles.end(),
                                      [&](const RegisteredConsole& c) { return c.hostId == hostId; });
    if (console == config_.consoles.end() || !console->credentialsValid) return;

    if (stateMachine_.state() == AppState::Error) stateMachine_.handle(AppEvent::ErrorAcknowledged);
    if (stateMachine_.state() != AppState::Idle) return;

    if (!options_.sessionFactory) {
        addNotice(Notice::Kind::Error, Notice::Topic::SessionSetupFailed, console->name,
                  "this build has no session support");
        return;
    }
    // Not found in this network, but Sony's servers know it: that way, without waking (it cannot).
    if (goesThroughSony(*console, Clock::now())) {
        if (!stateMachine_.handle(AppEvent::ConnectRequested)) return;
        beginSession(console->hostId, false, true);
        return;
    }
    const auto address = addressOf(*console);
    if (!address) {
        addNotice(Notice::Kind::Error, Notice::Topic::SessionSetupFailed, console->name,
                  "no address known for this console");
        return;
    }

    bool standby = false;
    for (const protocol::DiscoveredHost& host : discovered_) {
        if (equalsIgnoreCase(host.info.hostId, console->hostId)) {
            standby = host.info.state == protocol::HostState::Standby;
        }
    }
    if (standby) {
        const auto credential = protocol::wakeCredential(console->registKey);
        const auto ip = net::parseIpv4(*address);
        if (!credential || !ip) {
            addNotice(Notice::Kind::Error, Notice::Topic::WakeFailed, console->name);
            return;
        }
        if (!stateMachine_.wakeAndConnect()) return;
        const auto now = Clock::now();
        wake_ = WakeJob{console->hostId, *ip, *credential, now + options_.wakeTimeout, now};
        updateWake(now);
        return;
    }

    if (!stateMachine_.handle(AppEvent::ConnectRequested)) return;
    beginSession(console->hostId, false);
}

void AppController::disconnect() {
    if (stateMachine_.state() == AppState::Waking) {
        wake_.reset();
        stateMachine_.handle(AppEvent::DisconnectRequested);
        return;
    }
    if (!session_ || !stateMachine_.handle(AppEvent::DisconnectRequested)) return;

    session_->userStopped = true;
    if (session_->session) {
        session_->session->stop(); // the Quit event finishes the job in updateSession()
    } else {
        stateMachine_.handle(AppEvent::Disconnected);
        attachPump(nullptr);
        session_.reset();
    }
}

void AppController::sleepAndDisconnect() {
    if (!session_ || !session_->session || !session_->connected) return;
    const auto sent = session_->session->sleepConsole();
    if (!sent) {
        spdlog::warn("Rest mode request failed: {}", sent.error().message);
        addNotice(Notice::Kind::Warning, Notice::Topic::SleepFailed, session_->name, sent.error().message);
    }
    disconnect();
}

void AppController::submitLoginPin(std::string_view digits) {
    if (!session_ || !session_->session || !session_->loginPinRequested) return;
    const auto sent = session_->session->submitLoginPin(digits);
    if (!sent) {
        spdlog::warn("Login passcode was not accepted locally: {}", sent.error().message);
        return;
    }
    // Stays hidden unless the console asks again, which means it was wrong.
    session_->loginPinRequested = false;
}

void AppController::beginSession(std::string_view hostId, bool afterWake, bool viaPsn) {
    const auto console = std::find_if(config_.consoles.begin(), config_.consoles.end(),
                                      [&](const RegisteredConsole& c) { return c.hostId == hostId; });
    ActiveSession active;
    active.hostId = std::string(hostId);
    active.name = console != config_.consoles.end() ? console->name : std::string{};
    active.afterWake = afterWake;
    active.attempts = afterWake ? kAttemptsAfterWake : 1;
    active.viaPsn = viaPsn;
    attachPump(nullptr);
    session_ = std::move(active);
    if (!startAttempt()) failSession(session::QuitReason::SetupFailed, "session could not be started");
}

StreamSettings AppController::streamSettingsForAttempt() const {
    // With "automatic" the program's choice for this console, the stored settings otherwise.
    StreamSettings stream = effectiveStream(config_, session_ ? std::string_view(session_->hostId) : std::string_view{},
                                            session_ && session_->away);
    // HDR needs a display path that does not exist yet; the settings say so.
    stream.hdr = false;
    if (session_ && session_->bitrateKbps > 0) stream.bitrateKbps = session_->bitrateKbps;
    return stream;
}

void AppController::captureKey() {
    capturingKey_ = true;
    capturedKey_ = 0;
}

void AppController::endKeyCapture() {
    capturingKey_ = false;
    capturedKey_ = 0;
}

void AppController::keyCaptured(int code) {
    if (!capturingKey_) return;
    capturingKey_ = false;
    capturedKey_ = code == 0 ? -1 : code;
}

void AppController::applyStreamSettings() {
    if (!session_ || !session_->session || !session_->connected || session_->capture ||
        stateMachine_.state() != AppState::Streaming) {
        return;
    }
    spdlog::info("Connecting again to apply the stream settings");
    session_->restart = true;
    // A rate lowered after a stall belonged to the old settings.
    session_->bitrateKbps = 0;
    session_->stalls = 0;
    session_->session->stop(); // the Quit event starts the next attempt in handleQuit()
}

bool AppController::startAttempt() {
    const auto console = std::find_if(config_.consoles.begin(), config_.consoles.end(),
                                      [&](const RegisteredConsole& c) { return c.hostId == session_->hostId; });
    if (console == config_.consoles.end() || !console->credentialsValid || !options_.sessionFactory) return false;
    session::SessionConfig sessionConfig;
    std::string where;
    if (session_->viaPsn) {
        const session::PsnConsole* remote = psnUsable() ? psnConsoleFor(*console) : nullptr;
        const auto account = protocol::PsnAccountId::parse(config_.psn.accountId);
        if (!remote || !account) return false;
        sessionConfig.psn = session::SessionConfig::Psn{config_.psn.tokens.access, remote->id, account->bytes};
        if (session_->attempt <= 1) spdlog::info("{} is not in this network; going through Sony's servers", console->name);
        session_->away = true;
        where = "through Sony's servers";
    } else {
        const auto address = addressOf(*console);
        if (!address) return false;
        // At home or away decides which learned bitrate applies; said once, so a wrong guess shows in the log.
        const auto ip = net::parseIpv4(*address);
        const bool away = !session_->capture && options_.isAway && ip && options_.isAway(*ip);
        if (session_->attempt <= 1 || away != session_->away) {
            spdlog::info("Console at {}: {}", *address,
                         away ? "reached from away (through a VPN or another network); away bitrate and advice"
                              : "in this PC's home network");
        }
        session_->away = away;
        sessionConfig.host = *address;
        where = "at " + *address;
    }
    sessionConfig.registKey = console->registKey;
    sessionConfig.rpKey = console->rpKey;
    sessionConfig.stream = streamSettingsForAttempt();
    session_->requested = sessionConfig.stream;
    const auto controller = controllers_.active();
    const bool dualSense = controller && (controller->type == input::ControllerType::DualSense ||
                                          controller->type == input::ControllerType::DualSenseEdge);
    sessionConfig.dualSense = dualSense && config_.controller.adaptiveTriggers;

    auto created = options_.sessionFactory();
    if (!created) return false;
    // Full strength until the console says otherwise. That also undoes what another program left the controller at.
    controllers_.setEffectStrength(input::EffectStrength::Strong, input::EffectStrength::Strong);
    controllers_.clearTriggerEffects(); // and loose triggers: a test from the settings may still be on them
    spdlog::info("Connecting to {} {}, attempt {} of {} ({}x{}, {} fps, {} kbit/s)", console->name, where,
                 session_->attempt, session_->attempts, sessionConfig.stream.width, sessionConfig.stream.height,
                 sessionConfig.stream.fps, sessionConfig.stream.bitrateKbps);
    if (options_.mediaSink) options_.mediaSink->begin(sessionConfig.stream);
    const auto started = created->start(sessionConfig, options_.mediaSink);
    if (!started) {
        spdlog::error("Session could not be started: {}", started.error().message);
        if (options_.mediaSink) options_.mediaSink->end();
        return false;
    }
    attachPump(nullptr); // nothing of ours may still talk to a session this one replaces
    session_->session = std::move(created);
    session_->connected = false;
    session_->loginPinRequested = false;
    session_->loginPinWrong = false;
    session_->retryAt.reset();
    return true;
}

void AppController::updateSession(Clock::time_point now) {
    sessionActive_.store(session_ && session_->session && session_->connected);
    if (!session_) return;

    if (session_->retryAt && now >= *session_->retryAt) {
        session_->retryAt.reset();
        if (!startAttempt()) {
            failSession(session::QuitReason::SetupFailed, "session could not be started");
            return;
        }
    }
    if (!session_->session) return;

    if (session_->connected) {
        // With the pump running, input goes out from its thread, every few
        // milliseconds. Without it, once per call, as it always did.
        if (session_->capture) {
            // The controller belongs to the console on the other end of the card's cable.
        } else if (options_.inputInterval.count() > 0) {
            attachPump(session_->session.get());
        } else {
            session_->session->setControllerState(sessionInput(), now);
        }

        const session::SessionStats stats = session_->session->stats(now);
        const std::uint64_t frames = stats.videoFrames;
        if (frames != session_->videoFrames) {
            session_->videoFrames = frames;
            session_->lastPicture = now;
        }
        updateAlerts(stats, now);
        // A stream that stands still does not recover by waiting: on a link
        // that cannot carry the bitrate, the requests for repair get lost
        // too. Start over at half the rate instead.
        if (!session_->capture && !session_->stalled && now - session_->lastPicture >= options_.stallTimeout) {
            const int current = session_->requested.bitrateKbps;
            spdlog::warn("No picture for {} ms at {} kbit/s; reconnecting", options_.stallTimeout.count(), current);
            session_->stalled = true;
            // Stalls hours apart are no series: the session ends only when they follow each other.
            if (session_->lastStall != Clock::time_point{} && now - session_->lastStall > std::chrono::minutes(2)) {
                session_->stalls = 0;
            }
            ++session_->stalls;
            session_->lastStall = now;
            session_->bitrateKbps = std::max(kMinBitrateKbps, current / 2);
            session_->session->stop();
        }
    }

    for (session::SessionEvent& event : session_->session->pollEvents()) {
        switch (event.type) {
            case session::SessionEvent::Type::Connected:
                session_->connected = true;
                session_->everConnected = true;
                session_->connectedAt = now;
                session_->alerts.reset(now);
                session_->waitingForRelease = false;
                session_->releaseDeadline = {};
                session_->lastPicture = now;
                session_->videoFrames = 0;
                session_->attempt = 1;
                session_->attempts = 1;
                session_->loginPinRequested = false;
                if (stateMachine_.state() == AppState::Reconnecting) {
                    stateMachine_.handle(AppEvent::ReconnectSucceeded);
                } else {
                    stateMachine_.handle(AppEvent::SessionEstablished);
                }
                break;
            case session::SessionEvent::Type::LoginPinRequested:
                session_->loginPinRequested = true;
                session_->loginPinWrong = event.previousPinWrong;
                break;
            case session::SessionEvent::Type::Rumble:
                if (config_.controller.rumble) {
                    // 0..255 from the console, 0..65535 for the controller.
                    controllers_.rumble(static_cast<std::uint16_t>(event.rumbleLow * 257),
                                        static_cast<std::uint16_t>(event.rumbleHigh * 257), kRumbleDuration);
                }
                break;
            case session::SessionEvent::Type::TriggerEffects:
                if (config_.controller.adaptiveTriggers) {
                    controllers_.triggerEffects(event.triggers.typeLeft, event.triggers.left, event.triggers.typeRight,
                                                event.triggers.right);
                }
                break;
            case session::SessionEvent::Type::EffectStrength: {
                // What is set on the console under Accessories > Controllers.
                const input::EffectStrength haptics =
                    event.strengthOfTriggers ? controllers_.hapticStrength() : event.strength;
                const input::EffectStrength triggers =
                    event.strengthOfTriggers ? event.strength : controllers_.triggerStrength();
                static constexpr const char* kNames[] = {"off", "strong", "medium", "weak"};
                spdlog::info("The console sets the strength of {} to {}",
                             event.strengthOfTriggers ? "the adaptive triggers" : "vibration and haptics",
                             kNames[static_cast<int>(event.strength)]);
                controllers_.setEffectStrength(haptics, triggers);
                break;
            }
            case session::SessionEvent::Type::Quit:
                handleQuit(event.reason, event.detail, now);
                return; // the session object is gone
        }
    }
}

void AppController::handleQuit(session::QuitReason reason, const std::string& detail, Clock::time_point now) {
    spdlog::info("Session ended: {} ({})", session::toString(reason), detail);
    attachPump(nullptr);
    logLinkSummary(now);
    rememberLink(now);
    if (session_->connected) {
        lastEndedHostId_ = session_->hostId;
        lastEndedAt_ = now;
    }
    session_->session.reset(); // joins the session thread
    if (options_.mediaSink) options_.mediaSink->end();
    if (session_->connected) {
        // Leave the controller the way we found it.
        controllers_.rumble(0, 0, milliseconds{0});
        controllers_.clearTriggerEffects();
    }
    session_->connected = false;

    const AppState state = stateMachine_.state();
    if (std::exchange(session_->restart, false) && !session_->userStopped) {
        if (state == AppState::Streaming) stateMachine_.handle(AppEvent::ConnectionLost);
        session_->stalled = false;
        session_->attempt = 1;
        session_->attempts = kReconnectAttempts;
        session_->retryAt = now; // nothing to wait for: the console is there
        return;
    }
    if (std::exchange(session_->stalled, false) && !session_->userStopped) {
        if (session_->stalls > kMaxStalls) {
            failSession(session::QuitReason::ConnectionLost, "no picture arrived, even at the lowest bitrate");
            return;
        }
        addNotice(Notice::Kind::Warning, Notice::Topic::BitrateLowered,
                  std::to_string((session_->bitrateKbps + 500) / 1000));
        if (state == AppState::Streaming) stateMachine_.handle(AppEvent::ConnectionLost);
        session_->attempt = 1;
        session_->attempts = kReconnectAttempts;
        session_->retryAt = now; // nothing to wait for: the console is there
        return;
    }
    // "In use" right after a session of our own: the console has not let go
    // of that one yet. It does so by itself, so this is waited out instead
    // of reported; connecting again after a restart or a quick disconnect
    // would fail every time otherwise.
    const bool oursJustEnded = lastEndedHostId_ == session_->hostId && lastEndedAt_ != Clock::time_point{} &&
                               now - lastEndedAt_ < std::chrono::minutes(2);
    if (reason == session::QuitReason::InUse && !session_->userStopped && oursJustEnded) {
        if (session_->releaseDeadline == Clock::time_point{}) session_->releaseDeadline = now + options_.releaseWait;
        if (now < session_->releaseDeadline) {
            if (!session_->waitingForRelease) spdlog::info("The console still holds our last session; trying again until it lets go");
            session_->waitingForRelease = true;
            if (state == AppState::Streaming) stateMachine_.handle(AppEvent::ConnectionLost);
            session_->retryAt = now + options_.releaseRetry;
            return;
        }
    }
    session_->waitingForRelease = false;

    // A capture card that stops delivering is not reconnected like a console: that would only open it again.
    if (session_->capture && !session_->userStopped && reason != session::QuitReason::Stopped) {
        failSession(reason, detail);
        return;
    }

    // Rest mode ends a session from the console's side. That is an orderly
    // end, whether we asked for it or the user did it on the console.
    const bool orderly = session_->userStopped || reason == session::QuitReason::Stopped ||
                         (reason == session::QuitReason::ConsoleShutDown && session_->everConnected);
    if (orderly) {
        if (state != AppState::Disconnecting) stateMachine_.handle(AppEvent::DisconnectRequested);
        stateMachine_.handle(AppEvent::Disconnected);
        attachPump(nullptr);
        session_.reset();
        return;
    }

    if (state == AppState::Streaming) {
        stateMachine_.handle(AppEvent::ConnectionLost);
        session_->attempt = 1;
        session_->attempts = kReconnectAttempts;
        session_->retryAt = now + options_.retryDelay;
        return;
    }
    if (session::isTransient(reason) && session_->attempt < session_->attempts) {
        ++session_->attempt;
        session_->retryAt = now + options_.retryDelay;
        return;
    }
    failSession(reason, detail);
}

void AppController::graphicsRestored(Clock::time_point now) {
    if (session_) session_->alerts.graphicsRestored(now);
}

void AppController::updateAlerts(const session::SessionStats& stats, Clock::time_point now) {
    StreamObservation observation;
    observation.sincePicture = std::chrono::duration_cast<milliseconds>(now - session_->lastPicture);
    observation.framesLost = stats.framesLost;
    observation.framesDropped = droppedFrames_;
    observation.streamFps = stats.streamFps;
    observation.requestedFps = session_->requested.fps;
    const std::optional<input::ControllerInfo> pad = controllers_.active();
    observation.controllerConnected = pad.has_value();
    observation.batteryPercent = pad ? pad->batteryPercent : -1;
    observation.charging = pad && pad->charging;
    // With keys and mouse set up, no controller is no fault; with a capture card the controller is at the console.
    observation.controllerWanted = !config_.controller.keyboardMouse.enabled && !session_->capture;
    observation.pictureLatenessMs = session_->capture ? 0.0 : pictureLatenessMs_;
    if (!session_->capture && now - session_->connectedAt > std::chrono::seconds(3)) {
        session_->worstLatenessMs = std::max(session_->worstLatenessMs, pictureLatenessMs_);
        session_->latenessSum += pictureLatenessMs_;
        ++session_->latenessSamples;
        if (pictureInsideMs_ > 0.0) {
            session_->worstInsideMs = std::max(session_->worstInsideMs, pictureInsideMs_);
            session_->insideSum += pictureInsideMs_;
            ++session_->insideSamples;
        }
        if (stats.roundTripMs > 0.0) session_->roundTripMs = stats.roundTripMs;
    }
    session_->alerts.update(observation, now);
    // Into the log as well: afterwards it says what the user saw and when.
    for (const StreamAlert alert : session_->alerts.raised()) {
        const char* what = "";
        switch (alert) {
            case StreamAlert::NoController: what = "no controller connected"; break;
            case StreamAlert::ConnectionStalls: what = "no picture for a moment"; break;
            case StreamAlert::PicturesLost: what = "pictures lost on the way"; break;
            case StreamAlert::DecoderBehind: what = "the decoder threw pictures away"; break;
            case StreamAlert::FewerPictures: what = "the console sends fewer pictures than asked for"; break;
            case StreamAlert::BatteryLow: what = "the controller's battery is low"; break;
            case StreamAlert::GraphicsRestored: what = "the graphics device was made anew"; break;
            case StreamAlert::UnsteadyLink:
                ++session_->unsteadyNotes;
                what = session_->away ? "the console's pictures come unevenly (the way from away)"
                                      : "the console's pictures come unevenly (its Wi-Fi, most likely)";
                break;
        }
        spdlog::info("Note over the picture: {} ({} ms since the last picture, {:.0f} fps, {} lost, {} dropped)", what,
                     observation.sincePicture.count(), stats.streamFps, stats.framesLost, droppedFrames_);
    }
}

namespace {
// Pictures uneven by less than this on average make a steady link (a cable shows about 0).
constexpr double kSteadyLatenessMs = 2.0;
} // namespace

void AppController::logLinkSummary(Clock::time_point now) {
    if (!session_ || !session_->connected || session_->capture || session_->latenessSamples == 0) return;
    const double average = session_->latenessSum / session_->latenessSamples;
    const auto minutes = std::chrono::duration_cast<std::chrono::minutes>(now - session_->connectedAt).count();
    // What most likely held the picture back, in plain terms: this line is what is read afterwards.
    const char* verdict = average < kSteadyLatenessMs ? "steady"
                          : average < 6.0             ? "a little uneven"
                          : session_->away ? "uneven (the way from away: the upload at home or the line here)"
                                           : "uneven (the console's Wi-Fi, most likely: a cable or repeater helps)";
    const double inside = session_->insideSamples > 0 ? session_->insideSum / session_->insideSamples : 0.0;
    // Everything that makes an input come back later, so the log answers "why did it feel slow".
    spdlog::info("Link summary after {} min: {}; pictures held up {:.1f} ms on average, {:.1f} ms at worst ({} notes "
                 "about it over the picture). Delay: response {:.0f} ms at setup, {:.1f} ms inside the PC on average "
                 "({:.1f} at worst), of that {:.1f} ms on purpose at the end (frame generation and {:.1f} ms waiting "
                 "for late pictures)",
                 minutes, verdict, average, session_->worstLatenessMs, session_->unsteadyNotes, session_->roundTripMs,
                 inside, session_->worstInsideMs, pictureAddedMs_, pictureWaitedMs_);
}

void AppController::rememberLink(Clock::time_point now) {
    // Only "automatic" learns, and only from an attempt that reached the stream.
    if (!isAutomatic(config_) || !session_ || !session_->session || !session_->connected) return;
    const auto console = std::find_if(config_.consoles.begin(), config_.consoles.end(),
                                      [&](const RegisteredConsole& c) { return c.hostId == session_->hostId; });
    if (console == config_.consoles.end()) return;

    const session::SessionStats stats = session_->session->stats(now);
    SessionOutcome outcome;
    outcome.bitrateKbps = session_->requested.bitrateKbps;
    outcome.streamed = std::chrono::duration_cast<std::chrono::seconds>(now - session_->connectedAt);
    outcome.frames = stats.videoFrames;
    outcome.framesLost = stats.framesLost;
    outcome.stalled = session_->stalled;
    outcome.markLoweredBefore = session_->markLowered;
    // Away has its own memory: an evening over someone else's line must not spoil the rate at home.
    LinkMemory& memory = session_->away ? console->awayLink : console->link;
    const LinkMemory learned = learn(memory, outcome);
    if (learned.ceilingKbps != memory.ceilingKbps && learned.ceilingKbps > 0 &&
        (memory.ceilingKbps == 0 || learned.ceilingKbps < memory.ceilingKbps)) {
        session_->markLowered = true;
    }
    if (learned == memory) return;
    spdlog::info("Automatic bitrate for {}{}: {} kbit/s next time (this session {} kbit/s, {} of {} pictures lost{})",
                 console->name, session_->away ? " from away" : "", learned.bitrateKbps, outcome.bitrateKbps,
                 outcome.framesLost, outcome.frames + outcome.framesLost,
                 outcome.stalled ? ", stream stood still" : "");
    memory = learned;
    configDirty_ = true;
    saveAfter_ = Clock::now() + kSaveDelay;
}

void AppController::failSession(session::QuitReason reason, const std::string& detail) {
    addNotice(Notice::Kind::Error, noticeTopicFor(reason, session_->everConnected), session_->name, detail);
    const AppState state = stateMachine_.state();
    if (state == AppState::Streaming) stateMachine_.handle(AppEvent::ConnectionLost);
    if (state == AppState::Connecting) stateMachine_.handle(AppEvent::ConnectionFailed);
    else stateMachine_.handle(AppEvent::ReconnectFailed);
    attachPump(nullptr);
    session_.reset();
}

void AppController::handleQuickAction(input::QuickAction action) {
    switch (action) {
        case input::QuickAction::NextProfile: {
            // Automatic, then the two fixed ones, then automatic again.
            const std::array<std::string_view, 3> profiles = {kProfileAuto, kProfileSmooth30, kProfileNative60};
            std::size_t next = 0;
            for (std::size_t i = 0; i < profiles.size(); ++i) {
                if (profiles[i] == config_.activeProfile) next = (i + 1) % profiles.size();
            }
            selectProfile(profiles[next]);
            break;
        }
        case input::QuickAction::Sleep: sleepAndDisconnect(); break;
        case input::QuickAction::Disconnect: disconnect(); break;
        default: break;
    }
}

void AppController::forgetConsole(std::string_view hostId) {
    if (session_ && session_->hostId == hostId) disconnect();
    std::erase_if(config_.consoles, [&](const RegisteredConsole& c) { return c.hostId == hostId; });
    if (wake_ && wake_->hostId == hostId) {
        wake_.reset();
        stateMachine_.handle(AppEvent::DisconnectRequested);
    }
    updateDirectTargets();
    saveNow();
}

void AppController::startPairing(std::string_view address, std::string_view accountId, std::string_view pin) {
    if (pairing_.phase == PairingStatus::Phase::Running) return;

    const auto ip = net::parseIpv4(trim(address));
    const auto account = protocol::PsnAccountId::parse(accountId);
    const auto pinValue = protocol::parsePin(pin);
    if (!ip || !account || !pinValue) {
        pairing_ = {PairingStatus::Phase::Failed, {}, ErrorCode::InvalidArgument, "invalid address, account ID or PIN", {}};
        return;
    }

    // Remember the account ID so the next pairing does not ask for it again.
    config_.accountId = account->toBase64();
    configChanged();

    pairing_ = {PairingStatus::Phase::Running, {}, {}, {}, {}};
    pairingAddress_ = net::formatIpv4(*ip);

    protocol::RegistrationParams params;
    params.address = *ip;
    params.port = options_.registPort;
    params.account = *account;
    params.pin = *pinValue;

    if (pairingThread_.joinable()) pairingThread_.join();
    pairingThread_ = std::jthread([this, params](std::stop_token stop) {
        auto result = protocol::registerWithConsole(params, stop);
        std::lock_guard lock(pairingMutex_);
        pairingResult_ = std::move(result);
    });
}

void AppController::cancelPairing() {
    if (pairingThread_.joinable()) {
        pairingThread_.request_stop();
        pairingThread_.join();
    }
    std::lock_guard lock(pairingMutex_);
    pairingResult_.reset();
    pairing_ = {};
}

void AppController::dismissPairing() {
    if (pairing_.phase != PairingStatus::Phase::Running) pairing_ = {};
}

void AppController::dismissNotice(std::size_t index) {
    if (index >= notices_.size()) return;
    notices_.erase(notices_.begin() + static_cast<std::ptrdiff_t>(index));
    if (stateMachine_.state() == AppState::Error) stateMachine_.handle(AppEvent::ErrorAcknowledged);
}

void AppController::selectProfile(std::string_view profileId) {
    if (applyProfile(config_, profileId)) configChanged();
}

void AppController::configChanged() {
    sanitize(config_);
    applySettings();
    configDirty_ = true;
    saveAfter_ = Clock::now() + kSaveDelay;
}

void AppController::openHelp(std::string_view topic) {
    if (options_.openHelp) options_.openHelp(topic);
}

void AppController::startConnectionTest(std::string_view hostId) {
    // Paired or not, anything on the dashboard can be measured.
    for (const ConsoleEntry& entry : buildConsoleList(config_.consoles, discovered_)) {
        if (entry.hostId != hostId) continue;
        const auto address = net::parseIpv4(entry.address);
        if (!address) return;
        probeHostId_ = entry.hostId;
        probeName_ = entry.name;
        (void)probe_.start(*address);
        return;
    }
}

void AppController::stopConnectionTest() { probe_.stop(); }

void AppController::applyStreamAdvice() {
    if (!probe_.running()) return;
    const auto quality = protocol::rateLink(probe_.snapshot());
    if (quality == protocol::LinkQuality::Unknown) return;
    const protocol::StreamAdvice advice = protocol::adviceFor(quality);
    config_.stream.bitrateKbps = advice.bitrateKbps;
    // A fresh measurement replaces what "automatic" learned about this console.
    for (RegisteredConsole& console : config_.consoles) {
        if (console.hostId != probeHostId_) continue;
        console.link.bitrateKbps = std::min(advice.bitrateKbps, kAutoMaxBitrateKbps);
        console.link.ceilingKbps = 0;
    }
    config_.network.jitter = advice.stableBuffer ? JitterMode::Stable : JitterMode::LowLatency;
    configChanged();
}

void AppController::checkForUpdates() {
    if (updater_) updater_->check();
}

void AppController::installUpdate() {
    if (updater_) updater_->install();
}

void AppController::restart() {
    if (configDirty_) saveNow();
    if (options_.restart) options_.restart();
}

void AppController::takeScreenshot() {
    if (stateMachine_.state() != AppState::Streaming || !options_.takeScreenshot) return;
    options_.takeScreenshot();
}

void AppController::openPicturesFolder() {
    if (options_.openFolder && !options_.picturesFolder.empty()) options_.openFolder(options_.picturesFolder);
}

void AppController::captureWheel(int target) {
    if (target < 0 || target >= static_cast<int>(input::WheelTarget::Count)) return;
    const auto raw = controllers_.readWheel();
    if (!raw) return;
    // What the wheel shows right now is "at rest"; the movement from there decides.
    wheelCapture_.start(static_cast<input::WheelTarget>(target), *raw);
}

void AppController::endWheelCapture() { wheelCapture_.cancel(); }

void AppController::chooseWheel(const std::string& name) {
    input::WheelSettings& wheel = config_.controller.wheel;
    if (wheel.device == name) return;
    // Another wheel numbers its axes and buttons its own way: start over.
    const input::WheelSettings fresh;
    const bool enabled = wheel.enabled;
    const float centering = wheel.centering;
    wheel = fresh;
    wheel.device = name;
    wheel.enabled = enabled;
    wheel.centering = centering;
    wheelCapture_.cancel();
    configChanged();
}

void AppController::updateWheel() {
    // While a game streams, the wheel pulls back to the middle, as one does at a console with force feedback.
    const bool streaming = session_ && session_->connected && !session_->capture &&
                           stateMachine_.state() == AppState::Streaming;
    const input::WheelSettings& wheel = config_.controller.wheel;
    controllers_.setWheelCentering(streaming && wheel.enabled ? wheel.centering : 0.0f);

    if (!wheelCapture_.active()) return;
    const auto raw = controllers_.readWheel();
    if (!raw) {
        wheelCapture_.cancel();
        return;
    }
    const auto found = wheelCapture_.feed(*raw);
    if (!found) return;
    input::WheelSettings& settings = config_.controller.wheel;
    switch (found->target) {
        case input::WheelTarget::Steer:
            settings.steerAxis = found->index;
            settings.steerCenter = found->rest;
            break;
        case input::WheelTarget::Gas: settings.gas = {found->index, found->rest, found->extreme}; break;
        case input::WheelTarget::Brake: settings.brake = {found->index, found->rest, found->extreme}; break;
        default: {
            // One button presses one control: it leaves the control it had before.
            for (int& button : settings.buttons) {
                if (button == found->index) button = -1;
            }
            settings.buttons[static_cast<std::size_t>(static_cast<int>(found->target) -
                                                      static_cast<int>(input::WheelTarget::Cross))] = found->index;
            break;
        }
    }
    spdlog::info("Wheel: {} is {} {}", static_cast<int>(found->target),
                 found->target <= input::WheelTarget::Brake ? "axis" : "button", found->index);
    configChanged();
}

namespace {

/// The card's pictures, from the public repository. Discord takes an https address in place of an
/// art asset uploaded to the application (Activity Asset Image, "External URL assets"): a picture
/// that was never uploaded there, or under another name, showed as the application's icon instead.
std::string discordPicture(std::string_view name) {
    return "https://raw.githubusercontent.com/" + std::string(update::kReleaseRepository) + "/main/assets/discord/" +
           std::string(name) + ".png";
}

} // namespace

void AppController::updatePresence(Clock::time_point now) {
    if (!presence_ && options_.discordPipe) {
        presence_ = std::make_unique<discord::Presence>(options_.discordPipe(), options_.processId);
    }
    if (!presence_) return;
    const bool streaming = session_ && session_->connected && stateMachine_.state() == AppState::Streaming;
    if (!config_.discord.enabled || !streaming) {
        presenceKey_.clear();
        presence_->update(std::nullopt, now);
        return;
    }
    const ui::Strings& t =
        ui::strings(ui::resolveLanguage(config_.language, options_.systemLanguage));
    discord::Activity activity;
    if (session_->capture) {
        activity.game = t.captureCards;
        activity.platformImage = discordPicture("capture");
        activity.platformText = t.captureCards;
    } else {
        if (config_.discord.showGame) {
            // The console's own word first; what the user entered where it names none.
            activity.game = !sessionGameName_.empty() ? sessionGameName_ : trimmed(config_.discord.gameName);
        }
        activity.platformImage = discordPicture("ps5");
        activity.platformText = "PS5";
    }
    // "via VeloPlay" only under a game: alone it read "VeloPlay via VeloPlay" under the card's title.
    if (!activity.game.empty()) activity.state = t.presenceState;
    activity.appText = "VeloPlay";
    activity.appImage = discordPicture("veloplay");
    activity.buttonLabel = t.presenceButton;
    activity.buttonUrl = "https://github.com/" + std::string(update::kReleaseRepository);
    // The time runs from the start of the stream, and starts again with another game.
    const std::string key = session_->hostId + "|" + activity.game;
    if (key != presenceKey_) {
        presenceKey_ = key;
        presenceSince_ = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
    }
    activity.since = presenceSince_;
    presence_->update(activity, now);
}

void AppController::showCapture(const std::string& deviceId) {
    if (!options_.captureFactory || !options_.captureDevices) return;
    const std::vector<CaptureDevice> devices = options_.captureDevices();
    const auto device = std::find_if(devices.begin(), devices.end(), [&](const CaptureDevice& d) { return d.id == deviceId; });
    if (device == devices.end()) return;
    if (stateMachine_.state() == AppState::Error) stateMachine_.handle(AppEvent::ErrorAcknowledged);
    if (stateMachine_.state() != AppState::Idle || !stateMachine_.handle(AppEvent::ConnectRequested)) return;

    ActiveSession active;
    active.hostId = "capture:" + device->id;
    active.name = device->name;
    active.capture = true;
    attachPump(nullptr);
    session_ = std::move(active);

    session::SessionConfig sessionConfig;
    sessionConfig.host = device->id;
    sessionConfig.name = device->name;
    // What the card is asked for; it delivers what it can, and the renderer takes any size.
    sessionConfig.stream.width = 1920;
    sessionConfig.stream.height = 1080;
    sessionConfig.stream.fps = 60;
    // Nothing to compare the arriving rate with: a card's rate is the card's.
    session_->requested = sessionConfig.stream;
    session_->requested.fps = 0;
    auto created = options_.captureFactory();
    if (!created) {
        failSession(session::QuitReason::SetupFailed, "this build cannot show capture cards");
        return;
    }
    spdlog::info("Showing the capture card {}", device->name);
    if (options_.mediaSink) options_.mediaSink->begin(sessionConfig.stream);
    const auto started = created->start(sessionConfig, options_.mediaSink);
    if (!started) {
        spdlog::error("Capture card could not be opened: {}", started.error().message);
        if (options_.mediaSink) options_.mediaSink->end();
        failSession(session::QuitReason::SetupFailed, started.error().message);
        return;
    }
    session_->session = std::move(created);
}

void AppController::toggleRecording() {
    if (!options_.toggleRecording) return;
    // Ending one is always allowed; starting one only with a game on screen.
    const bool running = options_.recording && (options_.recording().running || options_.recording().starting);
    if (!running && stateMachine_.state() != AppState::Streaming) return;
    options_.toggleRecording();
}

void AppController::openVideosFolder() {
    if (options_.openFolder && !options_.videosFolder.empty()) options_.openFolder(options_.videosFolder);
}

void AppController::captureFinished(bool saved, std::string where, std::string detail, bool video) {
    const char* what = video ? "Video" : "Screenshot";
    if (saved) spdlog::info("{} saved in {}", what, where);
    else spdlog::warn("{} not saved: {}", what, detail);
    ++captureNote_.serial;
    captureNote_.video = video;
    captureNote_.saved = saved;
    captureNote_.where = std::move(where);
    captureNote_.detail = std::move(detail);
}

void AppController::installProgram() {
    if (!options_.installer || installing_.valid() || options_.installer->installed()) return;
    installError_.clear();
    // The installed program reads the same settings; what is still unsaved has to be in the file first.
    if (configDirty_) saveNow();
    installing_ = std::async(std::launch::async, [installer = options_.installer] { return installer->install(); });
}

void AppController::updateInstall() {
    if (!installing_.valid() || installing_.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    const Result<std::filesystem::path> installed = installing_.get();
    if (!installed) {
        installError_ = installed.error().message;
        spdlog::warn("Install: {}", installError_);
        return;
    }
    spdlog::info("Install: done, going on in {}", install::utf8(installed.value()));
    if (options_.startInstalled) options_.startInstalled(installed.value());
}

// ---- internals ---------------------------------------------------------

void AppController::addNotice(Notice::Kind kind, Notice::Topic topic, std::string detail, std::string technical) {
    // The same problem reported twice is still one problem.
    std::erase_if(notices_, [&](const Notice& notice) { return notice.topic == topic; });
    notices_.push_back({kind, topic, std::move(detail), std::move(technical)});
}

void AppController::windowAction(WindowAction action) {
    if (options_.windowAction) options_.windowAction(action);
}

void AppController::testVibration() {
    if (session_) return; // the console owns the controller's effects then
    // Both motors, clearly felt, short enough not to annoy.
    controllers_.rumble(0xA000, 0xA000, milliseconds{600});
}

void AppController::testTriggers(int effect) {
    if (session_) return;
    input::TriggerEffect chosen; // released
    switch (effect) {
        case 1: chosen = input::feedbackEffect(1, 8); break;     // nearly the whole travel, as strong as it gets
        case 2: chosen = input::weaponEffect(3, 6, 8); break;    // a point in the middle to push through
        case 3: chosen = input::vibrationEffect(1, 8, 30); break; // shakes while pressed
        default: break;
    }
    // Full strength, whatever a game left the controller at.
    controllers_.setEffectStrength(input::EffectStrength::Strong, input::EffectStrength::Strong);
    if (chosen.type == input::kTriggerEffectOff) {
        controllers_.clearTriggerEffects();
    } else {
        controllers_.triggerEffects(chosen.type, chosen.parameters, chosen.type, chosen.parameters);
    }
    spdlog::info("Trigger test: effect {}", effect);
}

void AppController::updateSessionGame() {
    // Which game runs on the console of the session decides the key layout.
    // The console says so in its answers to the search. While it does not
    // answer for a moment, the layout stays as it is: a lost packet must not
    // change the keys under the player's fingers.
    std::string id = sessionGameId_;
    std::string name = sessionGameName_;
    if (!session_) {
        id.clear();
        name.clear();
        sessionGameLogged_ = false;
    } else {
        for (const protocol::DiscoveredHost& host : discovered_) {
            if (!equalsIgnoreCase(host.info.hostId, session_->hostId) || host.info.state != protocol::HostState::Ready) continue;
            // An answer without a game keeps the one before for the session: the console may leave the
            // field out while it is being played remotely (Discord then showed no game at all).
            if (host.info.runningAppTitleId.empty() && !sessionGameId_.empty()) continue;
            id = host.info.runningAppTitleId;
            name = host.info.runningAppName;
        }
    }
    if (session_ && session_->connected && !sessionGameLogged_) {
        // Once per stream, whatever it is: Discord showed no game, and only this says whether the console names one.
        sessionGameLogged_ = true;
        const protocol::DiscoveredHost* console = nullptr;
        for (const protocol::DiscoveredHost& host : discovered_) {
            if (equalsIgnoreCase(host.info.hostId, session_->hostId)) console = &host;
        }
        spdlog::info("Game on the console at the start of the stream: {}",
                     !console ? std::string("the console has not answered the search")
                     : id.empty() ? std::string("none named in its answer")
                                  : name + " (" + id + ")");
    }
    if (id == sessionGameId_) return;
    sessionGameId_ = std::move(id);
    sessionGameName_ = std::move(name);
    if (session_) {
        spdlog::info("Game on the console: {}", sessionGameName_.empty() ? std::string("none named") :
                                                    sessionGameName_ + " (" + sessionGameId_ + ")");
    }
    const input::KeyboardMouseSettings& all = config_.controller.keyboardMouse;
    keyboardMouse_.configure(input::settingsFor(all, sessionGameId_));
    if (all.enabled && session_) {
        const bool own = input::gameLayout(all, sessionGameId_) != nullptr;
        spdlog::info("Keys and mouse: {} layout{}{}", own ? "the game's own" : "the general",
                     sessionGameName_.empty() ? "" : " for ", sessionGameName_);
    }
}

void AppController::applySettings() {
    controllers_.setDeadzone(config_.controller.deadzone);
    controllers_.setPreferredName(config_.controller.preferredName);
    controllers_.setWheel(config_.controller.wheel);
    keyboardMouse_.configure(input::settingsFor(config_.controller.keyboardMouse, sessionGameId_));
    updateDirectTargets();
    applyRemoteApi();
}

void AppController::applyRemoteApi() {
    if (remoteApiApplied_ && appliedRemoteApi_ == config_.remoteApi) return;
    // A new password needs no restart; it applies to the next request.
    const bool restart = !remoteApiApplied_ || appliedRemoteApi_.enabled != config_.remoteApi.enabled ||
                         appliedRemoteApi_.port != config_.remoteApi.port;
    remoteApiApplied_ = true;
    appliedRemoteApi_ = config_.remoteApi;

    remoteProcessor_.setToken(config_.remoteApi.token);
    if (!restart) return;
    remoteServer_.stop();
    virtualController_.releaseAll();
    if (!config_.remoteApi.enabled) return;

    const auto started = remoteServer_.start(config_.remoteApi.port);
    if (!started) {
        spdlog::error("Remote control API could not start: {}", started.error().message);
        addNotice(Notice::Kind::Error, Notice::Topic::RemoteApiFailed, std::to_string(config_.remoteApi.port));
    }
}

void AppController::updateDirectTargets() {
    std::vector<std::uint32_t> targets;
    for (const std::string& host : config_.manualHosts) {
        if (const auto ip = net::parseIpv4(host)) targets.push_back(*ip);
    }
    for (const RegisteredConsole& console : config_.consoles) {
        if (const auto ip = net::parseIpv4(console.lastAddress)) targets.push_back(*ip);
    }
    discovery_.setDirectTargets(std::move(targets));
}

void AppController::updateWake(Clock::time_point now) {
    if (!wake_) return;

    for (const protocol::DiscoveredHost& host : discovered_) {
        if (!equalsIgnoreCase(host.info.hostId, wake_->hostId)) continue;
        if (host.info.state == protocol::HostState::Ready) {
            const std::string hostId = wake_->hostId;
            wake_.reset();
            stateMachine_.handle(AppEvent::HostReady);
            if (stateMachine_.state() == AppState::Connecting) beginSession(hostId, true);
            return;
        }
    }

    if (now >= wake_->deadline) {
        const auto console = std::find_if(config_.consoles.begin(), config_.consoles.end(),
                                          [&](const RegisteredConsole& c) { return c.hostId == wake_->hostId; });
        addNotice(Notice::Kind::Error, Notice::Topic::WakeTimedOut,
                  console != config_.consoles.end() ? console->name : std::string{});
        wake_.reset();
        stateMachine_.handle(AppEvent::WakeTimedOut);
        return;
    }

    if (now >= wake_->nextSend) {
        wake_->nextSend = now + kWakeResendInterval;
        const auto sent = discovery_.wake(wake_->address, wake_->credential);
        if (!sent) spdlog::warn("Wake-up packet could not be sent: {}", sent.error().message);
    }
}

// --- Playing from away through Sony's servers ---

std::int64_t AppController::unixNow() const {
    if (options_.unixTime) return options_.unixTime();
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
        .count();
}

bool AppController::psnUsable() const {
    return static_cast<bool>(options_.psnConsoles) && config_.psn.signedIn() && !psnConsoles_.empty();
}

bool AppController::foundAtHome(std::string_view hostId) const {
    for (const protocol::DiscoveredHost& host : discovered_) {
        if (equalsIgnoreCase(host.info.hostId, hostId)) return true;
    }
    return false;
}

bool AppController::goesThroughSony(const RegisteredConsole& console, Clock::time_point now) const {
    if (!psnUsable()) return false;
    const session::PsnConsole* remote = psnConsoleFor(console);
    if (!remote || !remote->remotePlay) return false;
    if (options_.alwaysThroughSony) return true;
    if (foundAtHome(console.hostId)) return false;
    for (const auto& [hostId, seen] : seenAtHome_) {
        if (equalsIgnoreCase(hostId, console.hostId) && now - seen < options_.homeMemory) return false;
    }
    return true;
}

// Sony's list knows a console by its name and an id of its own, not by the id it answers searches with. The
// name matches when nobody renamed it since pairing; with one console on each side there is nothing to mix up.
const session::PsnConsole* AppController::psnConsoleFor(const RegisteredConsole& console) const {
    for (const session::PsnConsole& remote : psnConsoles_) {
        if (equalsIgnoreCase(remote.name, console.name)) return &remote;
    }
    if (psnConsoles_.size() == 1 && config_.consoles.size() == 1) return &psnConsoles_.front();
    return nullptr;
}

void AppController::startRemoteLogin() {
    psnError_.clear();
    psnExpired_ = false;
    if (!options_.psnConsoles || !options_.http || !options_.showLogin) {
        psnPhase_ = UiModel::PsnRemoteView::Phase::Failed;
        psnError_ = "playing from away is not available in this build";
        return;
    }
    const auto clientId = protocol::newPsnClientId();
    if (!clientId) {
        psnPhase_ = UiModel::PsnRemoteView::Phase::Failed;
        psnError_ = clientId.error().message;
        return;
    }
    if (accountLookup_) accountLookup_->cancel();
    remoteLogin_ = true;
    psnPhase_ = UiModel::PsnRemoteView::Phase::SigningIn;
    if (!options_.showLogin(protocol::psnRemoteLoginUrl(*clientId))) {
        remoteLogin_ = false;
        psnPhase_ = UiModel::PsnRemoteView::Phase::Failed;
        psnError_ = "the sign-in page could not be opened";
    }
}

void AppController::signOutRemote() {
    config_.psn = PsnRemoteSettings{};
    psnConsoles_.clear();
    psnListed_ = false;
    psnPhase_ = UiModel::PsnRemoteView::Phase::Idle;
    psnError_.clear();
    psnExpired_ = false;
    spdlog::info("Playing from away: signed out");
    saveNow();
}

void AppController::updatePsn(Clock::time_point now) {
    using Phase = UiModel::PsnRemoteView::Phase;
    const auto ready = [](const auto& future) {
        return future.valid() && future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    };

    if (ready(psnSignIn_)) {
        auto signedIn = psnSignIn_.get();
        if (!signedIn) {
            psnPhase_ = Phase::Failed;
            psnError_ = signedIn.error().message;
            spdlog::warn("Playing from away: sign-in failed: {}", signedIn.error().message);
        } else {
            config_.psn.tokens = signedIn->tokens;
            config_.psn.accountId = signedIn->account.toBase64();
            config_.psn.sealed.clear();
            // Pairing wants the same number; whoever signed in here owns the consoles.
            if (config_.accountId.empty()) config_.accountId = config_.psn.accountId;
            psnPhase_ = Phase::Idle;
            psnExpired_ = false;
            psnNextList_ = now; // look at once
            psnNextRefresh_ = {};
            spdlog::info("Playing from away: signed in");
            saveNow();
        }
    }

    if (ready(psnRefresh_)) {
        auto refreshed = psnRefresh_.get();
        if (refreshed) {
            config_.psn.tokens = *refreshed;
            config_.psn.sealed.clear();
            saveNow();
        } else if (refreshed.error().code == ErrorCode::Rejected &&
                   (refreshed.error().message.ends_with("HTTP 400") || refreshed.error().message.ends_with("HTTP 401"))) {
            // Sony no longer takes the refresh token (signed out elsewhere, or months unused). Anything else, a
            // server that is down or a hotel's sign-in page in its place, is tried again: the sign-in stays.
            spdlog::warn("Playing from away: the sign-in ran out: {}", refreshed.error().message);
            signOutRemote();
            psnExpired_ = true;
        } else {
            spdlog::info("Playing from away: renewing the sign-in failed, trying again: {}", refreshed.error().message);
            psnNextRefresh_ = now + options_.psnRetry;
        }
    }

    if (ready(psnList_)) {
        auto listed = psnList_.get();
        if (listed) {
            if (*listed != psnConsoles_) {
                spdlog::info("Playing from away: Sony knows {} console(s), {} with Remote Play on", listed->size(),
                             std::count_if(listed->begin(), listed->end(),
                                           [](const session::PsnConsole& c) { return c.remotePlay; }));
            }
            psnConsoles_ = std::move(*listed);
            psnListed_ = true;
        } else {
            spdlog::info("Playing from away: the console list could not be fetched: {}", listed.error().message);
        }
    }

    if (!options_.psnConsoles || !options_.http || !config_.psn.signedIn()) return;
    if (psnSignIn_.valid() || psnRefresh_.valid()) return;

    // Renew ten minutes before it runs out: a session asks with the access token when it starts.
    if (config_.psn.tokens.expiresAt - unixNow() < 600 && now >= psnNextRefresh_) {
        if (config_.psn.tokens.refresh.empty()) {
            signOutRemote();
            psnExpired_ = true;
            return;
        }
        update::IHttpClient* http = options_.http;
        psnRefresh_ = std::async(std::launch::async, [http, refresh = config_.psn.tokens.refresh, at = unixNow()] {
            return protocol::refreshPsnTokens(*http, refresh, at, {});
        });
        return;
    }

    // The list only matters while a paired console is not found here.
    const bool someoneMissing =
        options_.alwaysThroughSony || std::any_of(config_.consoles.begin(), config_.consoles.end(),
                                                  [&](const RegisteredConsole& c) { return !foundAtHome(c.hostId); });
    if (someoneMissing && !psnList_.valid() && now >= psnNextList_ && now - firstUpdate_ >= options_.psnFirstListAfter) {
        psnNextList_ = now + options_.psnRetry;
        psnList_ = std::async(std::launch::async,
                              [list = options_.psnConsoles, token = config_.psn.tokens.access] { return list(token); });
    }
}

void AppController::startAccountLogin() {
    accountStatus_ = AccountLookup{};
    if (!accountLookup_ || !options_.showLogin) {
        accountStatus_.phase = AccountLookup::Phase::Failed;
        accountStatus_.error = ErrorCode::InvalidArgument;
        accountStatus_.errorDetail = "signing in is not available in this build";
        return;
    }
    accountLookup_->cancel();
    accountStatus_.phase = AccountLookup::Phase::SigningIn;
    if (!options_.showLogin(protocol::psnLoginUrl())) {
        accountStatus_.phase = AccountLookup::Phase::Failed;
        accountStatus_.error = ErrorCode::Io;
        accountStatus_.errorDetail = "the sign-in page could not be opened";
    }
}

void AppController::startAccountLoginInBrowser() {
    accountStatus_ = AccountLookup{};
    if (!accountLookup_ || !options_.openAddress) {
        accountStatus_.phase = AccountLookup::Phase::Failed;
        accountStatus_.error = ErrorCode::InvalidArgument;
        accountStatus_.errorDetail = "signing in is not available in this build";
        return;
    }
    accountLookup_->cancel();
    // Waits like the window does; the address pasted afterwards goes through submitAccountRedirect().
    accountStatus_.phase = AccountLookup::Phase::SigningIn;
    options_.openAddress(protocol::psnLoginUrl());
}

void AppController::submitAccountRedirect(std::string_view url) {
    const auto code = protocol::psnCodeFromRedirect(url);
    if (remoteLogin_) {
        remoteLogin_ = false;
        if (!code || !options_.http) {
            psnPhase_ = UiModel::PsnRemoteView::Phase::Failed;
            psnError_ = "the address carries no sign-in code";
            return;
        }
        psnPhase_ = UiModel::PsnRemoteView::Phase::Fetching;
        update::IHttpClient* http = options_.http;
        psnSignIn_ = std::async(std::launch::async, [http, code = *code, now = unixNow()] {
            return protocol::signInForRemote(*http, code, now, {});
        });
        return;
    }
    accountStatus_ = AccountLookup{};
    if (!code || !accountLookup_) {
        accountStatus_.phase = AccountLookup::Phase::Failed;
        accountStatus_.error = ErrorCode::InvalidArgument;
        accountStatus_.errorDetail = code ? "signing in is not available in this build"
                                          : "the address carries no sign-in code";
        return;
    }
    accountLookup_->start(*code);
    accountStatus_.phase = AccountLookup::Phase::Fetching;
}

void AppController::accountLoginClosed() {
    if (remoteLogin_) {
        remoteLogin_ = false;
        if (psnPhase_ == UiModel::PsnRemoteView::Phase::SigningIn) psnPhase_ = UiModel::PsnRemoteView::Phase::Idle;
        return;
    }
    if (accountStatus_.phase == AccountLookup::Phase::SigningIn) accountStatus_ = AccountLookup{};
}

void AppController::dismissAccountLookup() {
    if (accountLookup_) accountLookup_->cancel();
    accountStatus_ = AccountLookup{};
}

void AppController::updateAccountLookup() {
    if (!accountLookup_ || accountStatus_.phase != AccountLookup::Phase::Fetching) return;
    const auto result = accountLookup_->take();
    if (!result) return;
    if (result->ok()) {
        accountStatus_.phase = AccountLookup::Phase::Succeeded;
        accountStatus_.accountId = (*result)->toBase64();
        config_.accountId = accountStatus_.accountId;
        configDirty_ = true;
        saveAfter_ = Clock::now();
        spdlog::info("Account ID received from Sony");
    } else {
        accountStatus_.phase = AccountLookup::Phase::Failed;
        accountStatus_.error = result->error().code;
        accountStatus_.errorDetail = result->error().message;
        spdlog::warn("Account lookup failed: {}", result->error().message);
    }
}

void AppController::updatePairing() {
    std::optional<Result<protocol::RegisteredHost>> result;
    {
        std::lock_guard lock(pairingMutex_);
        result.swap(pairingResult_);
    }
    if (!result) return;

    if (!result->ok()) {
        const Error& error = result->error();
        if (error.code == ErrorCode::Canceled) {
            pairing_ = {};
            return;
        }
        spdlog::warn("Pairing failed: {}", error.message);
        pairing_ = {PairingStatus::Phase::Failed, {}, error.code, error.message, {}};
        return;
    }

    RegisteredConsole console = RegisteredConsole::from(result->value(), pairingAddress_);
    // The discovery name is what the user sees on the console itself.
    for (const protocol::DiscoveredHost& host : discovered_) {
        if (equalsIgnoreCase(host.info.hostId, console.hostId) && !host.info.hostName.empty()) {
            console.name = host.info.hostName;
        }
    }

    pairing_ = {PairingStatus::Phase::Succeeded, console.name, {}, {}, console.hostId};
    std::erase_if(config_.consoles, [&](const RegisteredConsole& c) { return c.hostId == console.hostId; });
    config_.consoles.push_back(std::move(console));
    updateDirectTargets();
    saveNow();
}

void AppController::updateControllers() {
    controllers_.update();
    for (const auto& event : controllers_.takeEvents()) {
        const bool connected = event.kind == input::ControllerManager::Event::Kind::Connected;
        // The header shows the controller at all times, so the log line is enough.
        spdlog::info("Controller {}: {}", connected ? "connected" : "disconnected", event.info.name);
    }
}

void AppController::updateRemoteStatus(Clock::time_point now) {
    if (!remoteServer_.running() || now < nextRemoteStatus_) return;
    nextRemoteStatus_ = now + kRemoteStatusInterval;

    json status;
    status["state"] = toString(stateMachine_.state());
    status["profile"] = config_.activeProfile;
    if (session_) {
        status["session"] = {{"console", session_->name}, {"connected", session_->connected}};
    } else {
        status["session"] = nullptr;
    }
    if (const auto active = controllers_.active()) {
        status["controller"] = active->name;
    } else {
        status["controller"] = nullptr;
    }
    json consoles = json::array();
    for (const ConsoleEntry& entry : buildConsoleList(config_.consoles, discovered_)) {
        consoles.push_back({{"name", entry.name},
                            {"address", entry.address},
                            {"status", statusName(entry.status)},
                            {"paired", entry.registered}});
    }
    status["consoles"] = std::move(consoles);

    std::lock_guard lock(remoteMutex_);
    remoteStatusJson_ = status.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

void AppController::saveNow() {
    configDirty_ = false;
    if (!store_) return;
    const auto saved = store_->save(config_);
    if (!saved) {
        spdlog::error("Saving settings failed: {}", saved.error().message);
        addNotice(Notice::Kind::Error, Notice::Topic::ConfigSaveFailed, saved.error().message);
    }
}

} // namespace veloplay
