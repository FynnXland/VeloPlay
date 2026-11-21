#include "app/AppController.hpp"

#include <algorithm>
#include <array>
#include <utility>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "input/InputMerger.hpp"
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
    // The pump goes first: it reads the controllers and talks to the session.
    if (pump_.joinable()) {
        pump_.request_stop();
        pump_.join();
    }
    if (session_ && session_->session) {
        attachPump(nullptr);
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

    diag::setStep("reading the controllers");
    updateControllers();
    diag::setStep("wake-up");
    updateWake(now);
    diag::setStep("session");
    updateSession(now);
    diag::setStep("pairing, remote control, settings");
    updatePairing();
    updateAccountLookup();

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
    }
    model.discoveryRunning = discoveryRunning_;

    const auto active = controllers_.active();
    model.controllerConnected = active.has_value();
    model.controller = active.value_or(input::ControllerInfo{});
    model.controllers = controllers_.controllers();
    model.controllerState = controllers_.state();

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
        if (session_->session && session_->connected) {
            const session::SessionStats stats = session_->session->stats(now);
            model.session.videoFrames = stats.videoFrames;
            model.session.audioPackets = stats.audioPackets;
            model.session.framesLost = stats.framesLost;
            model.session.receivedMegabytes = static_cast<double>(stats.videoBytes) / 1'000'000.0;
            model.session.connectedSeconds = static_cast<int>(
                std::chrono::duration_cast<std::chrono::seconds>(now - session_->connectedAt).count());
            model.session.settingsChanged = !(session_->requested == streamSettingsForAttempt());

            StreamStats view;
            view.presenting = false; // decoding and display arrive with the next milestone
            view.streamFps = static_cast<float>(stats.streamFps);
            view.bitrateMbps = static_cast<float>(stats.bitrateMbps);
            view.roundTripMs = static_cast<float>(stats.roundTripMs);
            view.packetLossPercent = static_cast<float>(stats.packetLossPercent);
            model.stream = std::move(view);
        }
    }

    model.updaterAvailable = updater_ != nullptr;
    if (updater_) model.update = updater_->status();

    model.remoteApiRunning = remoteServer_.running();
    model.capturingKey = capturingKey_;
    model.capturedKey = capturedKey_;
    model.config = &config_;
    // What a session asks for: with the console of the running session, or
    // else the first paired one. The settings screen shows it.
    model.requestedStream = effectiveStream(config_, session_ ? std::string_view(session_->hostId)
                                            : config_.consoles.empty() ? std::string_view{}
                                                                       : std::string_view(config_.consoles.front().hostId));
    model.requestedStream.hdr = false;
}

void AppController::attachPump(session::ISession* session) {
    std::lock_guard lock(pumpMutex_);
    pumpSession_ = session;
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
            input::mergeInputs(input::mergeInputs(controllers_.readNow(), keyboardMouse_.state(now)),
                               virtualController_.state(now)),
            now);
    }
}

input::ControllerState AppController::sessionInput() {
    const auto now = Clock::now();
    return input::mergeInputs(input::mergeInputs(controllers_.state(), keyboardMouse_.state(now)),
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

void AppController::beginSession(std::string_view hostId, bool afterWake) {
    const auto console = std::find_if(config_.consoles.begin(), config_.consoles.end(),
                                      [&](const RegisteredConsole& c) { return c.hostId == hostId; });
    ActiveSession active;
    active.hostId = std::string(hostId);
    active.name = console != config_.consoles.end() ? console->name : std::string{};
    active.afterWake = afterWake;
    active.attempts = afterWake ? kAttemptsAfterWake : 1;
    session_ = std::move(active);
    if (!startAttempt()) failSession(session::QuitReason::SetupFailed, "session could not be started");
}

StreamSettings AppController::streamSettingsForAttempt() const {
    // With "automatic" the program's choice for this console, the stored settings otherwise.
    StreamSettings stream = effectiveStream(config_, session_ ? std::string_view(session_->hostId) : std::string_view{});
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
    if (!session_ || !session_->session || !session_->connected || stateMachine_.state() != AppState::Streaming) {
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
    const auto address = addressOf(*console);
    if (!address) return false;

    session::SessionConfig sessionConfig;
    sessionConfig.host = *address;
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
    spdlog::info("Connecting to {} at {}, attempt {} of {} ({}x{}, {} fps, {} kbit/s)", console->name, *address,
                 session_->attempt, session_->attempts, sessionConfig.stream.width, sessionConfig.stream.height,
                 sessionConfig.stream.fps, sessionConfig.stream.bitrateKbps);
    if (options_.mediaSink) options_.mediaSink->begin(sessionConfig.stream);
    const auto started = created->start(sessionConfig, options_.mediaSink);
    if (!started) {
        spdlog::error("Session could not be started: {}", started.error().message);
        if (options_.mediaSink) options_.mediaSink->end();
        return false;
    }
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
        if (options_.inputInterval.count() > 0) attachPump(session_->session.get());
        else session_->session->setControllerState(sessionInput(), now);

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
        if (!session_->stalled && now - session_->lastPicture >= options_.stallTimeout) {
            const int current = session_->requested.bitrateKbps;
            spdlog::warn("No picture for {} ms at {} kbit/s; reconnecting", options_.stallTimeout.count(), current);
            session_->stalled = true;
            ++session_->stalls;
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

void AppController::updateAlerts(const session::SessionStats& stats, Clock::time_point now) {
    StreamObservation observation;
    observation.sincePicture = std::chrono::duration_cast<milliseconds>(now - session_->lastPicture);
    observation.framesLost = stats.framesLost;
    observation.framesDropped = droppedFrames_;
    observation.streamFps = stats.streamFps;
    observation.requestedFps = session_->requested.fps;
    observation.controllerConnected = controllers_.active().has_value();
    // With keys and mouse set up, no controller is no fault.
    observation.controllerWanted = !config_.controller.keyboardMouse.enabled;
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
        }
        spdlog::info("Note over the picture: {} ({} ms since the last picture, {:.0f} fps, {} lost, {} dropped)", what,
                     observation.sincePicture.count(), stats.streamFps, stats.framesLost, droppedFrames_);
    }
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
    const LinkMemory learned = learn(console->link, outcome);
    if (learned == console->link) return;
    spdlog::info("Automatic bitrate for {}: {} kbit/s next time (this session {} kbit/s, {} of {} pictures lost{})",
                 console->name, learned.bitrateKbps, outcome.bitrateKbps, outcome.framesLost,
                 outcome.frames + outcome.framesLost, outcome.stalled ? ", stream stood still" : "");
    console->link = learned;
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
        pairing_ = {PairingStatus::Phase::Failed, {}, ErrorCode::InvalidArgument, "invalid address, account ID or PIN"};
        return;
    }

    // Remember the account ID so the next pairing does not ask for it again.
    config_.accountId = account->toBase64();
    configChanged();

    pairing_ = {PairingStatus::Phase::Running, {}, {}, {}};
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

// ---- internals ---------------------------------------------------------

void AppController::addNotice(Notice::Kind kind, Notice::Topic topic, std::string detail, std::string technical) {
    // The same problem reported twice is still one problem.
    std::erase_if(notices_, [&](const Notice& notice) { return notice.topic == topic; });
    notices_.push_back({kind, topic, std::move(detail), std::move(technical)});
}

void AppController::applySettings() {
    controllers_.setDeadzone(config_.controller.deadzone);
    controllers_.setPreferredName(config_.controller.preferredName);
    keyboardMouse_.configure(config_.controller.keyboardMouse);
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

void AppController::submitAccountRedirect(std::string_view url) {
    const auto code = protocol::psnCodeFromRedirect(url);
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
        pairing_ = {PairingStatus::Phase::Failed, {}, error.code, error.message};
        return;
    }

    RegisteredConsole console = RegisteredConsole::from(result->value(), pairingAddress_);
    // The discovery name is what the user sees on the console itself.
    for (const protocol::DiscoveredHost& host : discovered_) {
        if (equalsIgnoreCase(host.info.hostId, console.hostId) && !host.info.hostName.empty()) {
            console.name = host.info.hostName;
        }
    }

    pairing_ = {PairingStatus::Phase::Succeeded, console.name, {}, {}};
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
    remoteStatusJson_ = status.dump();
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
