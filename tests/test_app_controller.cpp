#include <gtest/gtest.h>

#include <thread>

#include <nlohmann/json.hpp>

#include "FakeProtector.hpp"
#include "FakeSession.hpp"
#include "MockConsole.hpp"
#include "app/AppController.hpp"

using namespace veloplay;
using namespace std::chrono_literals;

namespace {

struct CountingSink final : session::IMediaSink {
    int begins = 0;
    int ends = 0;
    StreamSettings lastStream{};
    void begin(const StreamSettings& stream) override {
        ++begins;
        lastStream = stream;
    }
    void end() override { ++ends; }
    bool onVideo(std::span<const std::uint8_t>, int, bool) override { return true; }
    void onAudioFormat(const session::AudioFormat&) override {}
    void onAudio(std::span<const std::uint8_t>) override {}
};

/// A microphone that records what the test hands it.
class FakeMicrophone final : public media::IAudioInput {
public:
    bool start(Sink sink) override {
        ++starts;
        if (failStart) return false;
        sink_ = std::move(sink);
        running_ = true;
        return true;
    }
    void stop() override {
        ++stops;
        sink_ = nullptr;
        running_ = false;
    }
    [[nodiscard]] bool running() const override { return running_; }
    [[nodiscard]] bool recording() const override { return running_ && haveDevice; }

    /// One block of a tone, as the device would deliver it. Returns false if nobody listens.
    bool record() {
        if (!sink_) return false;
        std::vector<std::int16_t> block(static_cast<std::size_t>(media::kMicFrameSamples * media::kMicChannels));
        for (std::size_t i = 0; i < block.size(); ++i) block[i] = static_cast<std::int16_t>((i % 96) * 300 - 14000);
        sink_(block);
        return true;
    }
    /// The device vanishes, as when it is unplugged.
    void unplug() { running_ = false; }

    bool failStart = false;
    bool haveDevice = true; // false: started, but no microphone is plugged in
    int starts = 0;
    int stops = 0;

private:
    Sink sink_;
    bool running_ = false;
};

struct Harness {
    CountingSink sink;
    FakeMicrophone microphone;
    test::FakeProtector protector;
    std::filesystem::path configPath;
    std::shared_ptr<test::FakeSessionHub> sessions = std::make_shared<test::FakeSessionHub>();
    std::unique_ptr<AppController> app;
    UiModel model;

    Harness(const test::MockConsole& console, const char* fileName, bool removeOld = true,
            std::chrono::milliseconds stallTimeout = 60s, std::chrono::microseconds inputInterval = 0us,
            std::unique_ptr<input::IControllerBackend> backend = nullptr,
            const std::function<void(AppController::Options&)>& adjust = {}) {
        const auto dir = std::filesystem::temp_directory_path() / "veloplay_tests";
        std::filesystem::create_directories(dir);
        configPath = dir / fileName;
        if (removeOld) std::filesystem::remove(configPath);

        AppController::Options options;
        options.configPath = configPath;
        options.protector = &protector;
        options.discovery.consolePort = console.discoveryPort();
        options.discovery.localPortFirst = 0;
        options.discovery.searchInterval = 50ms;
        options.discovery.hostTimeout = 500ms;
        options.discovery.broadcast = false;
        options.registPort = console.registPort();
        options.wakeTimeout = 3000ms;
        options.sessionFactory = sessions->factory(sessions);
        options.retryDelay = 20ms;
        options.releaseRetry = 20ms;
        options.releaseWait = 600ms;
        options.mediaSink = &sink;
        options.stallTimeout = stallTimeout;
        options.inputInterval = inputInterval;
        options.controllerBackend = std::move(backend);
        options.microphone = &microphone;
        options.microphoneRetry = 20ms;
        if (adjust) adjust(options);
        app = std::make_unique<AppController>(std::move(options));
    }

    /// Starts the client and pairs it with the mock console.
    bool pair() {
        app->start();
        app->config().manualHosts = {"127.0.0.1"};
        app->configChanged();
        if (!runUntil([&] { return !model.consoles.empty(); })) return false;
        app->startPairing("127.0.0.1", "ASNFZ4mrze8=", "12345678");
        if (!runUntil([&] { return model.pairing.phase != PairingStatus::Phase::Running; })) return false;
        app->dismissPairing();
        return model.pairing.phase == PairingStatus::Phase::Succeeded;
    }

    [[nodiscard]] bool hasNotice(Notice::Topic topic) const {
        for (const Notice& notice : model.notices) {
            if (notice.topic == topic) return true;
        }
        return false;
    }

    static constexpr const char* kHostId = "001122AABBCC";

    /// Runs frames until `condition` holds.
    template <class Condition>
    bool runUntil(Condition condition, std::chrono::milliseconds timeout = 4000ms) {
        return test::waitFor(
            [&] {
                app->update();
                app->fillModel(model);
                return condition();
            },
            timeout);
    }
};

} // namespace

/// An installer that copies nothing: it says what it was asked and can be told to fail.
struct FakeInstaller final : IInstaller {
    bool isInstalled = false;
    bool fail = false;
    std::atomic<int> calls{0};

    [[nodiscard]] bool installed() const override { return isInstalled; }
    [[nodiscard]] std::filesystem::path target() const override { return "programs/VeloPlay"; }
    Result<std::filesystem::path> install() override {
        ++calls;
        if (fail) return Error{ErrorCode::Io, "the disk is full"};
        return std::filesystem::path("programs/VeloPlay/VeloPlay.exe");
    }
};

TEST(AppController, InstallsAndGoesOnInTheInstalledProgram) {
    test::MockConsole console(test::MockConsole::Config{});
    FakeInstaller installer;
    std::filesystem::path started;
    Harness h(console, "controller_install.json", true, 60s, 0us, nullptr, [&](AppController::Options& options) {
        options.installer = &installer;
        options.startInstalled = [&](const std::filesystem::path& program) { started = program; };
    });
    h.app->start();
    h.app->fillModel(h.model);
    EXPECT_TRUE(h.model.install.available);
    EXPECT_FALSE(h.model.install.installed);
    EXPECT_EQ(h.model.install.target, std::filesystem::path("programs/VeloPlay").string());

    // An attempt that fails says why and leaves the program where it runs.
    installer.fail = true;
    h.app->installProgram();
    ASSERT_TRUE(h.runUntil([&] { return !h.model.install.busy && !h.model.install.error.empty(); }));
    EXPECT_EQ(h.model.install.error, "the disk is full");
    EXPECT_TRUE(started.empty());

    // One that works hands over to the installed program.
    installer.fail = false;
    h.app->installProgram();
    ASSERT_TRUE(h.runUntil([&] { return !started.empty(); }));
    EXPECT_EQ(started, std::filesystem::path("programs/VeloPlay/VeloPlay.exe"));
    EXPECT_TRUE(h.model.install.error.empty());
    EXPECT_EQ(installer.calls.load(), 2);

    // The installed program does not install itself again.
    installer.isInstalled = true;
    h.app->installProgram();
    h.app->update();
    h.app->fillModel(h.model);
    EXPECT_TRUE(h.model.install.installed);
    EXPECT_EQ(installer.calls.load(), 2);
}

TEST(AppController, PairsWakesAndRemembersAConsole) {
    test::MockConsole::Config consoleConfig;
    consoleConfig.standby = true;
    test::MockConsole console(consoleConfig);

    {
        Harness h(console, "controller_flow.json");
        h.app->start();
        h.app->config().manualHosts = {"127.0.0.1"};
        h.app->configChanged();

        // 1. The console shows up, unpaired and in rest mode.
        ASSERT_TRUE(h.runUntil([&] { return !h.model.consoles.empty(); }));
        EXPECT_FALSE(h.model.consoles[0].registered);
        EXPECT_EQ(h.model.consoles[0].status, ConsoleStatus::Standby);
        EXPECT_EQ(h.model.consoles[0].name, "PS5-042");

        // 2. Pairing with the PIN from the console.
        h.app->startPairing("127.0.0.1", "ASNFZ4mrze8=", "1234 5678");
        h.app->fillModel(h.model);
        EXPECT_EQ(h.model.pairing.phase, PairingStatus::Phase::Running);
        ASSERT_TRUE(h.runUntil([&] { return h.model.pairing.phase != PairingStatus::Phase::Running; }));
        ASSERT_EQ(h.model.pairing.phase, PairingStatus::Phase::Succeeded) << h.model.pairing.errorDetail;
        EXPECT_EQ(h.model.pairing.consoleName, "PS5-042");
        ASSERT_EQ(h.model.consoles.size(), 1u);
        EXPECT_TRUE(h.model.consoles[0].registered);
        EXPECT_TRUE(h.model.consoles[0].credentialsValid);
        EXPECT_EQ(h.app->config().accountId, "ASNFZ4mrze8=");

        h.app->dismissPairing();
        h.app->fillModel(h.model);
        EXPECT_EQ(h.model.pairing.phase, PairingStatus::Phase::Idle);

        // 3. Wake-up: the stored key yields the credential the console expects.
        h.app->wake(h.model.consoles[0].hostId);
        h.app->fillModel(h.model);
        EXPECT_EQ(h.model.state, AppState::Waking);
        EXPECT_TRUE(h.model.consoles[0].waking);
        ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Idle; }));
        EXPECT_EQ(h.model.consoles[0].status, ConsoleStatus::Ready);
        EXPECT_FALSE(h.model.consoles[0].waking);
        EXPECT_GE(console.wakeupsReceived(), 1);
    }

    // 4. A new start finds the paired console in the settings file.
    Harness again(console, "controller_flow.json", false);
    again.app->start();
    again.app->fillModel(again.model);
    ASSERT_EQ(again.model.consoles.size(), 1u);
    EXPECT_TRUE(again.model.consoles[0].registered);
    EXPECT_TRUE(again.model.consoles[0].credentialsValid);
    EXPECT_EQ(again.model.consoles[0].hostId, "001122AABBCC");
}

TEST(AppController, ReportsWrongPinAndInvalidInput) {
    test::MockConsole console({});
    Harness h(console, "controller_wrong_pin.json");
    h.app->start();

    h.app->startPairing("127.0.0.1", "ASNFZ4mrze8=", "00000000");
    ASSERT_TRUE(h.runUntil([&] { return h.model.pairing.phase != PairingStatus::Phase::Running; }));
    EXPECT_EQ(h.model.pairing.phase, PairingStatus::Phase::Failed);
    EXPECT_EQ(h.model.pairing.error, ErrorCode::Rejected);
    EXPECT_TRUE(h.app->config().consoles.empty());

    h.app->dismissPairing();
    h.app->startPairing("not an address", "ASNFZ4mrze8=", "12345678");
    h.app->fillModel(h.model);
    EXPECT_EQ(h.model.pairing.phase, PairingStatus::Phase::Failed);
    EXPECT_EQ(h.model.pairing.error, ErrorCode::InvalidArgument);
}

TEST(AppController, WakeTimeoutRaisesANoticeAndRecovers) {
    test::MockConsole::Config consoleConfig;
    consoleConfig.standby = true;
    consoleConfig.wakeCredential = 0xdead; // console will ignore our packets
    test::MockConsole console(consoleConfig);

    Harness h(console, "controller_wake_timeout.json");
    h.app->start();
    h.app->startPairing("127.0.0.1", "ASNFZ4mrze8=", "12345678");
    ASSERT_TRUE(h.runUntil([&] { return h.model.pairing.phase == PairingStatus::Phase::Succeeded; }));

    h.app->wake("001122AABBCC");
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Error; }, 6000ms));
    ASSERT_EQ(h.model.notices.size(), 1u);
    EXPECT_EQ(h.model.notices[0].topic, Notice::Topic::WakeTimedOut);
    EXPECT_EQ(h.model.notices[0].detail, "PS5-042");

    h.app->dismissNotice(0);
    h.app->fillModel(h.model);
    EXPECT_EQ(h.model.state, AppState::Idle);
    EXPECT_TRUE(h.model.notices.empty());
}

TEST(AppController, ForgetConsoleRemovesItFromDisk) {
    test::MockConsole console({});
    Harness h(console, "controller_forget.json");
    h.app->start();
    h.app->startPairing("127.0.0.1", "ASNFZ4mrze8=", "12345678");
    ASSERT_TRUE(h.runUntil([&] { return h.model.pairing.phase == PairingStatus::Phase::Succeeded; }));

    h.app->forgetConsole("001122AABBCC");
    EXPECT_TRUE(h.app->config().consoles.empty());

    ConfigStore store(h.configPath, h.protector);
    const auto onDisk = store.load();
    ASSERT_TRUE(onDisk);
    EXPECT_TRUE(onDisk->consoles.empty());
}

TEST(AppController, RemoteApiFollowsSettingsAndReportsStatus) {
    test::MockConsole console({});
    Harness h(console, "controller_remote.json");
    h.app->start();
    h.app->fillModel(h.model);
    EXPECT_FALSE(h.model.remoteApiRunning); // off unless the user enables it

    // Find a free port, then hand it to the application.
    std::uint16_t port = 0;
    {
        auto probe = net::UdpSocket::open({{net::kLoopbackAddress, 0}, false});
        ASSERT_TRUE(probe);
        port = probe->localEndpoint().port;
    }
    h.app->config().remoteApi.enabled = true;
    h.app->config().remoteApi.port = port;
    h.app->configChanged();
    ASSERT_TRUE(h.runUntil([&] { return h.model.remoteApiRunning; }));

    auto client = net::UdpSocket::open({{net::kLoopbackAddress, 0}, false});
    ASSERT_TRUE(client);
    const auto ask = [&](std::string_view request) {
        (void)client->sendTo(asBytes(request), {net::kLoopbackAddress, port});
        std::optional<net::Datagram> answer;
        h.runUntil([&] {
            auto received = client->receive(10ms);
            if (received && received->has_value()) answer = std::move(**received);
            return answer.has_value();
        });
        return answer ? nlohmann::json::parse(asText(answer->data)) : nlohmann::json::object();
    };

    const auto status = ask(R"({"cmd":"status"})");
    ASSERT_TRUE(status.value("ok", false)) << status.dump();
    EXPECT_EQ(status["status"]["state"], "idle");
    EXPECT_EQ(status["status"]["profile"], "auto");

    // Scripted input reaches the session input.
    ask(R"({"cmd":"press","buttons":["cross"],"duration_ms":3000})");
    EXPECT_TRUE(h.app->sessionInput().pressed(input::Button::Cross));

    // Profile switching works without a session, the rest says why it cannot.
    ASSERT_TRUE(ask(R"({"cmd":"action","name":"next_profile"})").value("ok", false));
    ASSERT_TRUE(h.runUntil([&] { return h.app->config().activeProfile == "smooth30"; }));
    EXPECT_EQ(ask(R"({"cmd":"action","name":"sleep"})").value("error", ""), "no_active_session");

    h.app->config().remoteApi.enabled = false;
    h.app->configChanged();
    h.app->fillModel(h.model);
    EXPECT_FALSE(h.model.remoteApiRunning);
    EXPECT_FALSE(h.app->sessionInput().pressed(input::Button::Cross)); // nothing stays held
}

TEST(AppController, MeasuresTheLinkAndAppliesAdvice) {
    test::MockConsole console({});
    Harness h(console, "controller_link.json");
    h.app->start();
    h.app->config().manualHosts = {"127.0.0.1"};
    h.app->configChanged();
    ASSERT_TRUE(h.runUntil([&] { return !h.model.consoles.empty(); }));
    EXPECT_FALSE(h.model.connectionTest.running);
    EXPECT_FALSE(h.model.updaterAvailable); // no HTTP client was passed in

    // Works for a console that is not paired: only discovery packets are used.
    h.app->startConnectionTest(h.model.consoles[0].hostId);
    h.app->fillModel(h.model);
    ASSERT_TRUE(h.model.connectionTest.running);
    EXPECT_EQ(h.model.connectionTest.consoleName, "PS5-042");

    h.app->stopConnectionTest();
    h.app->fillModel(h.model);
    EXPECT_FALSE(h.model.connectionTest.running);

    h.app->applyStreamAdvice(); // no measurement running: settings stay as they are
    EXPECT_EQ(h.app->config().stream.bitrateKbps, 15'000);
}

// ---- sessions -----------------------------------------------------------

TEST(AppControllerSession, ConnectsStreamsAndDisconnects) {
    test::MockConsole console({});
    Harness h(console, "session_basic.json");
    ASSERT_TRUE(h.pair());
    // Settings chosen by hand are passed on as they are.
    h.app->config().activeProfile = std::string(kProfileCustom);
    h.app->config().stream.bitrateKbps = 12'000;
    h.app->config().stream.fps = 60;

    h.app->connect(Harness::kHostId);
    h.app->fillModel(h.model);
    EXPECT_EQ(h.model.state, AppState::Connecting);
    EXPECT_EQ(h.model.session.consoleName, "PS5-042");
    EXPECT_FALSE(h.model.stream.has_value());

    // The session got the paired keys and the stream settings.
    ASSERT_EQ(h.sessions->configs.size(), 1u);
    const session::SessionConfig& config = h.sessions->configs[0];
    EXPECT_EQ(config.host, "127.0.0.1");
    EXPECT_EQ(config.registKey, test::MockConsole::registKeyAscii());
    EXPECT_EQ(config.rpKey, h.app->config().consoles[0].rpKey);
    EXPECT_EQ(config.stream.bitrateKbps, 12'000);
    EXPECT_EQ(config.stream.fps, 60);
    EXPECT_FALSE(config.dualSense); // no controller connected

    h.sessions->stats.streamFps = 59.9;
    h.sessions->stats.bitrateMbps = 11.5;
    h.sessions->stats.roundTripMs = 4.0;
    h.sessions->stats.packetLossPercent = 0.5;
    h.sessions->stats.videoFrames = 600;
    h.sessions->stats.videoBytes = 14'000'000;
    h.sessions->emit(test::FakeSessionHub::connected());
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));

    ASSERT_TRUE(h.model.stream.has_value());
    EXPECT_FALSE(h.model.stream->presenting);
    EXPECT_NEAR(h.model.stream->streamFps, 59.9f, 0.01f);
    EXPECT_NEAR(h.model.stream->bitrateMbps, 11.5f, 0.01f);
    EXPECT_NEAR(h.model.stream->packetLossPercent, 0.5f, 0.01f);
    EXPECT_EQ(h.model.session.videoFrames, 600u);
    EXPECT_NEAR(h.model.session.receivedMegabytes, 14.0, 0.01);

    // Input reaches the console every frame while the stream runs.
    const int before = h.sessions->controllerUpdates;
    h.app->update();
    h.app->update();
    EXPECT_EQ(h.sessions->controllerUpdates, before + 2);

    // A second connect while one runs changes nothing.
    h.app->connect(Harness::kHostId);
    EXPECT_EQ(h.sessions->configs.size(), 1u);

    h.app->disconnect();
    h.app->fillModel(h.model);
    EXPECT_EQ(h.model.state, AppState::Disconnecting);
    EXPECT_EQ(h.sessions->stops, 1);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Idle; }));
    EXPECT_FALSE(h.model.stream.has_value());
    EXPECT_TRUE(h.model.notices.empty());
    EXPECT_EQ(h.sessions->destroyed, 1);
}

TEST(AppControllerSession, WakesAConsoleInRestModeFirst) {
    test::MockConsole::Config consoleConfig;
    consoleConfig.standby = true;
    test::MockConsole console(consoleConfig);
    Harness h(console, "session_wake.json");
    ASSERT_TRUE(h.pair());

    // Fresh out of rest mode the console refuses twice before it is ready.
    h.sessions->scripts.push_back({test::FakeSessionHub::quit(session::QuitReason::ConnectionRefused)});
    h.sessions->scripts.push_back({test::FakeSessionHub::quit(session::QuitReason::ConnectionRefused)});
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});

    h.app->connect(Harness::kHostId);
    h.app->fillModel(h.model);
    EXPECT_EQ(h.model.state, AppState::Waking);
    EXPECT_TRUE(h.sessions->configs.empty());

    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    EXPECT_GE(console.wakeupsReceived(), 1);
    EXPECT_EQ(h.sessions->configs.size(), 3u);
    EXPECT_TRUE(h.model.notices.empty());
}

TEST(AppControllerSession, DoesNotRetryAConsoleThatIsAwake) {
    test::MockConsole console({});
    Harness h(console, "session_refused.json");
    ASSERT_TRUE(h.pair());
    h.sessions->scripts.push_back({test::FakeSessionHub::quit(session::QuitReason::ConnectionRefused, "refused")});

    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Error; }));
    EXPECT_EQ(h.sessions->configs.size(), 1u);
    ASSERT_EQ(h.model.notices.size(), 1u);
    EXPECT_EQ(h.model.notices[0].topic, Notice::Topic::SessionRefused);
    EXPECT_EQ(h.model.notices[0].detail, "PS5-042");
    EXPECT_EQ(h.model.notices[0].technical, "refused");

    // Trying again is possible straight from the error.
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
}

TEST(AppControllerSession, NamesTheReasonWhenTheConsoleSaysNo) {
    const std::pair<session::QuitReason, Notice::Topic> cases[] = {
        {session::QuitReason::InUse, Notice::Topic::SessionInUse},
        {session::QuitReason::VersionMismatch, Notice::Topic::SessionVersionMismatch},
        {session::QuitReason::ConsoleCrashed, Notice::Topic::SessionConsoleCrashed},
        {session::QuitReason::SetupFailed, Notice::Topic::SessionSetupFailed},
    };
    test::MockConsole console({});
    Harness h(console, "session_reasons.json");
    ASSERT_TRUE(h.pair());
    for (const auto& [reason, topic] : cases) {
        h.sessions->scripts.push_back({test::FakeSessionHub::quit(reason)});
        h.app->connect(Harness::kHostId);
        ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Error; }));
        EXPECT_TRUE(h.hasNotice(topic)) << session::toString(reason);
        h.app->dismissNotice(0);
        h.app->fillModel(h.model);
        EXPECT_EQ(h.model.state, AppState::Idle);
    }
}

namespace {

/// Records what goes to Discord; READY comes right after the handshake.
struct DiscordLog {
    std::vector<nlohmann::json> activities; // the args of every SET_ACTIVITY
};

struct DiscordPipe final : discord::IPipe {
    explicit DiscordPipe(std::shared_ptr<DiscordLog> log) : log_(std::move(log)) {}
    bool open() override { return true; }
    void close() override {}
    bool write(std::span<const std::uint8_t> bytes) override {
        partial_.insert(partial_.end(), bytes.begin(), bytes.end());
        for (auto& message : discord::takeMessages(partial_)) {
            if (message.opcode == discord::Opcode::Handshake) {
                const auto ready = discord::frame(discord::Opcode::Frame, {{"evt", "READY"}});
                pending_.insert(pending_.end(), ready.begin(), ready.end());
            } else if (message.body.value("cmd", "") == "SET_ACTIVITY") {
                log_->activities.push_back(message.body["args"]);
            }
        }
        return true;
    }
    std::optional<std::vector<std::uint8_t>> read() override { return std::exchange(pending_, {}); }

private:
    std::shared_ptr<DiscordLog> log_;
    std::vector<std::uint8_t> partial_;
    std::vector<std::uint8_t> pending_;
};

} // namespace

TEST(AppControllerSession, ShowsTheStreamInDiscordAndClearsItAfterwards) {
    test::MockConsole console({});
    auto log = std::make_shared<DiscordLog>();
    Harness h(console, "session_discord.json", true, 60s, 0us, nullptr, [&](AppController::Options& options) {
        options.discordPipe = [log] { return std::unique_ptr<discord::IPipe>(std::make_unique<DiscordPipe>(log)); };
        options.processId = 77;
        options.systemLanguage = "de";
    });
    ASSERT_TRUE(h.pair());
    EXPECT_TRUE(h.model.discordAvailable);
    EXPECT_TRUE(h.app->config().discord.enabled); // on unless switched off
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    ASSERT_TRUE(h.runUntil([&] { return !log->activities.empty(); }));
    const nlohmann::json& shown = log->activities[0];
    EXPECT_EQ(shown["pid"], 77);
    EXPECT_EQ(shown["activity"]["state"], "über VeloPlay");
    // The pictures by address from the public repository: an art asset that was never uploaded to
    // Discord's application showed as its icon instead.
    EXPECT_EQ(shown["activity"]["assets"]["large_image"],
              "https://raw.githubusercontent.com/FynnXland/VeloPlay/main/assets/discord/ps5.png");
    EXPECT_EQ(shown["activity"]["assets"]["small_image"],
              "https://raw.githubusercontent.com/FynnXland/VeloPlay/main/assets/discord/veloplay.png");
    EXPECT_GT(shown["activity"]["timestamps"]["start"].get<std::int64_t>(), 1'700'000'000);

    h.app->disconnect();
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Idle; }));
    // The card is cleared at once, not after Discord's 15 seconds.
    ASSERT_TRUE(h.runUntil([&] { return log->activities.size() >= 2; }));
    EXPECT_FALSE(log->activities.back().contains("activity"));
}

TEST(AppControllerSession, PutsTheGameAtTheTopOfTheDiscordCardOrNothingAtAll) {
    test::MockConsole console({});
    console.setNamesGame(false); // not every PS5 says what runs
    auto log = std::make_shared<DiscordLog>();
    Harness h(console, "session_discord_game.json", true, 60s, 0us, nullptr, [&](AppController::Options& options) {
        options.discordPipe = [log] { return std::unique_ptr<discord::IPipe>(std::make_unique<DiscordPipe>(log)); };
        options.systemLanguage = "de";
    });
    ASSERT_TRUE(h.pair());
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    ASSERT_TRUE(h.runUntil([&] { return !log->activities.empty(); }));
    // No game: no text under the card's title, neither "PS5" (the picture says so) nor "via VeloPlay"
    // (that read "VeloPlay via VeloPlay").
    const nlohmann::json& bare = log->activities.back()["activity"];
    EXPECT_FALSE(bare.contains("details"));
    EXPECT_FALSE(bare.contains("state"));
    EXPECT_EQ(bare["assets"]["large_image"], "https://raw.githubusercontent.com/FynnXland/VeloPlay/main/assets/discord/ps5.png");
}

TEST(AppControllerSession, PutsWhatTheUserEnteredAtTheTopOfTheDiscordCard) {
    test::MockConsole console({});
    console.setNamesGame(false);
    auto log = std::make_shared<DiscordLog>();
    Harness h(console, "session_discord_entered.json", true, 60s, 0us, nullptr, [&](AppController::Options& options) {
        options.discordPipe = [log] { return std::unique_ptr<discord::IPipe>(std::make_unique<DiscordPipe>(log)); };
        options.systemLanguage = "de";
    });
    ASSERT_TRUE(h.pair());
    h.app->config().discord.gameName = "  Mein Spiel ";
    h.app->configChanged();
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    ASSERT_TRUE(h.runUntil([&] { return !log->activities.empty(); }));
    const nlohmann::json& named = log->activities.back()["activity"];
    EXPECT_EQ(named["details"], "Mein Spiel");
    EXPECT_EQ(named["state"], "über VeloPlay");
    EXPECT_EQ(named["status_display_type"], 2);
}

TEST(AppControllerSession, ShowsNothingInDiscordWhenSwitchedOff) {
    test::MockConsole console({});
    auto log = std::make_shared<DiscordLog>();
    Harness h(console, "session_discord_off.json", true, 60s, 0us, nullptr, [&](AppController::Options& options) {
        options.discordPipe = [log] { return std::unique_ptr<discord::IPipe>(std::make_unique<DiscordPipe>(log)); };
    });
    ASSERT_TRUE(h.pair());
    h.app->config().discord.enabled = false;
    h.app->configChanged();
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    for (int i = 0; i < 10; ++i) h.app->update();
    EXPECT_TRUE(log->activities.empty());
}

TEST(AppControllerSession, ShowsACaptureCardWithoutConsoleParts) {
    test::MockConsole console({});
    auto cards = std::make_shared<test::FakeSessionHub>();
    Harness h(console, "session_capture.json", true, 200ms, 0us, nullptr, [&](AppController::Options& options) {
        options.captureDevices = [] { return std::vector<CaptureDevice>{{"usb#card", "USB3 Capture"}}; };
        options.captureFactory = cards->factory(cards);
    });
    h.app->start();
    ASSERT_TRUE(h.runUntil([&] { return !h.model.captureDevices.empty(); }));
    EXPECT_EQ(h.model.captureDevices[0].name, "USB3 Capture");

    // An unknown card does nothing.
    h.app->showCapture("usb#other");
    EXPECT_TRUE(cards->configs.empty());

    cards->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->showCapture("usb#card");
    ASSERT_EQ(cards->configs.size(), 1u);
    EXPECT_EQ(cards->configs[0].host, "usb#card");
    EXPECT_TRUE(h.sessions->configs.empty()); // no console session
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    EXPECT_TRUE(h.model.session.capture);
    EXPECT_EQ(h.model.session.consoleName, "USB3 Capture");

    // No input goes to a card, and a card without pictures is not reconnected at half the rate.
    for (int i = 0; i < 5; ++i) h.app->update();
    EXPECT_EQ(cards->inputs(), 0);
    std::this_thread::sleep_for(300ms);
    h.app->update();
    EXPECT_EQ(cards->stops, 0);
    // Changed stream settings are no reason to open the card again.
    h.app->applyStreamSettings();
    EXPECT_EQ(cards->stops, 0);

    // A card that goes away ends it with a notice, without trying again.
    cards->emit(test::FakeSessionHub::quit(session::QuitReason::ConnectionLost));
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Error; }));
    EXPECT_EQ(cards->configs.size(), 1u);
}

TEST(AppControllerSession, ReconnectsWhenTheStreamBreaks) {
    test::MockConsole console({});
    Harness h(console, "session_reconnect.json");
    ASSERT_TRUE(h.pair());
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));

    // First retry fails too, the second one works.
    h.sessions->scripts.push_back({test::FakeSessionHub::quit(session::QuitReason::SetupFailed)});
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.sessions->emit(test::FakeSessionHub::quit(session::QuitReason::ConnectionLost));
    h.app->update();
    h.app->fillModel(h.model);
    EXPECT_EQ(h.model.state, AppState::Reconnecting);
    EXPECT_EQ(h.model.session.attempt, 1);
    EXPECT_EQ(h.model.session.attempts, 3);
    EXPECT_FALSE(h.model.stream.has_value());

    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    EXPECT_EQ(h.sessions->configs.size(), 3u);
    EXPECT_TRUE(h.model.notices.empty());
}

TEST(AppControllerSession, GivesUpAfterThreeFailedReconnects) {
    test::MockConsole console({});
    Harness h(console, "session_lost.json");
    ASSERT_TRUE(h.pair());
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));

    for (int i = 0; i < 3; ++i) {
        h.sessions->scripts.push_back({test::FakeSessionHub::quit(session::QuitReason::ConnectionRefused)});
    }
    h.sessions->emit(test::FakeSessionHub::quit(session::QuitReason::ConnectionLost));
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Error; }));
    EXPECT_EQ(h.sessions->configs.size(), 4u);
    EXPECT_TRUE(h.hasNotice(Notice::Topic::SessionLost));
}

TEST(AppControllerSession, CanBeCancelledBetweenReconnects) {
    test::MockConsole console({});
    Harness h(console, "session_cancel.json");
    ASSERT_TRUE(h.pair());
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));

    h.sessions->emit(test::FakeSessionHub::quit(session::QuitReason::ConnectionLost));
    h.app->update();
    ASSERT_EQ(h.app->state(), AppState::Reconnecting);
    h.app->disconnect(); // no session exists at this moment
    EXPECT_EQ(h.app->state(), AppState::Idle);
    std::this_thread::sleep_for(40ms);
    h.app->update();
    EXPECT_EQ(h.sessions->configs.size(), 1u);
}

TEST(AppControllerSession, TreatsRestModeAsANormalEnd) {
    test::MockConsole console({});
    Harness h(console, "session_shutdown.json");
    ASSERT_TRUE(h.pair());
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));

    h.sessions->emit(test::FakeSessionHub::quit(session::QuitReason::ConsoleShutDown));
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Idle; }));
    EXPECT_TRUE(h.model.notices.empty());
    EXPECT_EQ(h.sessions->configs.size(), 1u);
}

TEST(AppControllerSession, SendsTheConsoleToRestMode) {
    test::MockConsole console({});
    Harness h(console, "session_sleep.json");
    ASSERT_TRUE(h.pair());
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));

    h.app->sleepAndDisconnect();
    EXPECT_EQ(h.sessions->sleeps, 1);
    EXPECT_EQ(h.sessions->stops, 1);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Idle; }));
    EXPECT_TRUE(h.model.notices.empty());

    // If the request cannot be sent, the session still ends and the user is told.
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.sessions->failSleep = true;
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    h.app->sleepAndDisconnect();
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Idle; }));
    EXPECT_TRUE(h.hasNotice(Notice::Topic::SleepFailed));
}

TEST(AppControllerSession, PassesTheLoginPasscodeOn) {
    test::MockConsole console({});
    Harness h(console, "session_pin.json");
    ASSERT_TRUE(h.pair());
    h.sessions->scripts.push_back({test::FakeSessionHub::loginPin(false)});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.session.loginPinRequested; }));
    EXPECT_FALSE(h.model.session.loginPinWrong);
    EXPECT_EQ(h.model.state, AppState::Connecting);

    h.app->submitLoginPin("0000");
    h.app->fillModel(h.model);
    EXPECT_FALSE(h.model.session.loginPinRequested);
    ASSERT_EQ(h.sessions->pins.size(), 1u);
    EXPECT_EQ(h.sessions->pins[0], "0000");

    // Wrong: the console asks again and says so.
    h.sessions->emit(test::FakeSessionHub::loginPin(true));
    ASSERT_TRUE(h.runUntil([&] { return h.model.session.loginPinRequested; }));
    EXPECT_TRUE(h.model.session.loginPinWrong);

    h.app->submitLoginPin("4321");
    h.sessions->emit(test::FakeSessionHub::connected());
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    EXPECT_FALSE(h.model.session.loginPinRequested);
}

TEST(AppControllerSession, ReportsAStartFailure) {
    test::MockConsole console({});
    Harness h(console, "session_start_failure.json");
    ASSERT_TRUE(h.pair());
    h.sessions->failStart = true;
    h.app->connect(Harness::kHostId);
    h.app->fillModel(h.model);
    EXPECT_EQ(h.model.state, AppState::Error);
    EXPECT_TRUE(h.hasNotice(Notice::Topic::SessionSetupFailed));
}

TEST(AppControllerSession, EndsTheSessionWhenTheConsoleIsForgotten) {
    test::MockConsole console({});
    Harness h(console, "session_forget.json");
    ASSERT_TRUE(h.pair());
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));

    h.app->forgetConsole(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Idle; }));
    EXPECT_EQ(h.sessions->stops, 1);
    EXPECT_TRUE(h.app->config().consoles.empty());
}

TEST(AppControllerSession, StopsARunningSessionOnShutdown) {
    test::MockConsole console({});
    auto hub = std::make_shared<test::FakeSessionHub>();
    {
        Harness h(console, "session_shutdown_app.json");
        hub = h.sessions;
        ASSERT_TRUE(h.pair());
        h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
        h.app->connect(Harness::kHostId);
        ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    }
    EXPECT_EQ(hub->destroyed, 1);
}

TEST(AppControllerSession, RemoteControlCanEndTheSession) {
    test::MockConsole console({});
    Harness h(console, "session_remote.json");
    ASSERT_TRUE(h.pair());

    std::uint16_t port = 0;
    {
        auto probe = net::UdpSocket::open({{net::kLoopbackAddress, 0}, false});
        ASSERT_TRUE(probe);
        port = probe->localEndpoint().port;
    }
    h.app->config().remoteApi.enabled = true;
    h.app->config().remoteApi.port = port;
    h.app->configChanged();
    ASSERT_TRUE(h.runUntil([&] { return h.model.remoteApiRunning; }));

    auto client = net::UdpSocket::open({{net::kLoopbackAddress, 0}, false});
    ASSERT_TRUE(client);
    const auto ask = [&](std::string_view request) {
        (void)client->sendTo(asBytes(request), {net::kLoopbackAddress, port});
        std::optional<net::Datagram> answer;
        h.runUntil([&] {
            auto received = client->receive(10ms);
            if (received && received->has_value()) answer = std::move(**received);
            return answer.has_value();
        });
        return answer ? nlohmann::json::parse(asText(answer->data)) : nlohmann::json::object();
    };

    EXPECT_EQ(ask(R"({"cmd":"action","name":"disconnect"})").value("error", ""), "no_active_session");

    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));

    // The status names the console, scripted input reaches it.
    nlohmann::json status;
    ASSERT_TRUE(h.runUntil([&] {
        status = ask(R"({"cmd":"status"})");
        return status["status"]["session"].is_object();
    }));
    EXPECT_EQ(status["status"]["state"], "streaming");
    EXPECT_EQ(status["status"]["session"]["console"], "PS5-042");
    ask(R"({"cmd":"press","buttons":["cross"],"duration_ms":3000})");
    ASSERT_TRUE(h.runUntil([&] { return h.sessions->lastInput.pressed(input::Button::Cross); }));

    // Not there yet, and said so.
    EXPECT_EQ(ask(R"({"cmd":"action","name":"toggle_microphone"})").value("error", ""), "not_available");

    ASSERT_TRUE(ask(R"({"cmd":"action","name":"sleep"})").value("ok", false));
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Idle; }));
    EXPECT_EQ(h.sessions->sleeps, 1);
}

TEST(AppControllerSession, TellsTheMediaSinkAboutEverySession) {
    test::MockConsole console({});
    Harness h(console, "session_sink.json");
    ASSERT_TRUE(h.pair());
    h.app->config().stream.hdr = true;
    h.app->config().stream.fps = 60;
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    EXPECT_EQ(h.sink.begins, 1);
    EXPECT_EQ(h.sink.ends, 0);
    EXPECT_EQ(h.sink.lastStream.fps, 60);
    // HDR cannot be shown yet, so it is not requested either.
    EXPECT_FALSE(h.sink.lastStream.hdr);
    EXPECT_FALSE(h.sessions->configs[0].stream.hdr);

    // A reconnect is a new session for the sink: old decoder out, new one in.
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.sessions->emit(test::FakeSessionHub::quit(session::QuitReason::ConnectionLost));
    ASSERT_TRUE(h.runUntil([&] { return h.sink.begins == 2 && h.model.state == AppState::Streaming; }));
    EXPECT_EQ(h.sink.ends, 1);

    h.app->disconnect();
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Idle; }));
    EXPECT_EQ(h.sink.ends, 2);
}

namespace {

/// One DualSense whose buttons a test presses from outside.
class ScriptedPad final : public input::IControllerBackend {
public:
    struct Shared {
        std::mutex mutex;
        input::ControllerState state{};
        int refreshes = 0;
    };
    explicit ScriptedPad(std::shared_ptr<Shared> shared) : shared_(std::move(shared)) {}

    void poll() override {}
    void refresh() override {
        std::lock_guard lock(shared_->mutex);
        ++shared_->refreshes;
    }
    std::vector<input::ControllerInfo> controllers() const override {
        input::ControllerInfo info;
        info.id = 7;
        info.name = "DualSense";
        info.type = input::ControllerType::DualSense;
        info.connection = input::ConnectionType::Usb;
        return {info};
    }
    std::optional<input::ControllerState> read(std::uint32_t id) override {
        if (id != 7) return std::nullopt;
        std::lock_guard lock(shared_->mutex);
        return shared_->state;
    }
    bool rumble(std::uint32_t, std::uint16_t, std::uint16_t, std::chrono::milliseconds) override { return true; }
    bool sendEffect(std::uint32_t, std::span<const std::uint8_t>) override { return true; }

private:
    std::shared_ptr<Shared> shared_;
};

} // namespace

TEST(AppControllerSession, PassesAButtonPressOnWithoutWaitingForTheNextPicture) {
    test::MockConsole console({});
    auto pad = std::make_shared<ScriptedPad::Shared>();
    Harness h(console, "session_pump.json", true, 60s, 1000us, std::make_unique<ScriptedPad>(pad));
    ASSERT_TRUE(h.pair());
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    // One more round, so the pump knows the session.
    h.app->update();

    // From here on nobody calls update(): that is the window busy drawing, or
    // waiting for the screen. The press has to reach the console anyway.
    const int before = h.sessions->inputs();
    {
        std::lock_guard lock(pad->mutex);
        pad->state.buttons |= input::mask(input::Button::Cross);
        pad->state.r2 = 200;
    }
    ASSERT_TRUE(test::waitFor(
        [&] { return (h.sessions->input().buttons & input::mask(input::Button::Cross)) != 0; }, 2000ms));
    EXPECT_EQ(h.sessions->input().r2, 200);
    EXPECT_GT(h.sessions->inputs(), before);

    // It keeps reading: releasing arrives the same way.
    {
        std::lock_guard lock(pad->mutex);
        pad->state.buttons = 0;
    }
    ASSERT_TRUE(test::waitFor(
        [&] { return (h.sessions->input().buttons & input::mask(input::Button::Cross)) == 0; }, 2000ms));

    // After the session ended the pump leaves the session alone.
    h.app->disconnect();
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Idle; }));
    const int after = h.sessions->inputs();
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(h.sessions->inputs(), after);
}

namespace {

/// No gamepad, one steering wheel whose axes and buttons a test moves.
class ScriptedWheel final : public input::IControllerBackend {
public:
    struct Shared {
        std::mutex mutex;
        input::RawJoystick raw;
        float centering = 0.0f;
        int springCalls = 0;
    };
    explicit ScriptedWheel(std::shared_ptr<Shared> shared) : shared_(std::move(shared)) {}
    void poll() override {}
    std::vector<input::ControllerInfo> controllers() const override { return {}; }
    std::optional<input::ControllerState> read(std::uint32_t) override { return std::nullopt; }
    bool rumble(std::uint32_t, std::uint16_t, std::uint16_t, std::chrono::milliseconds) override { return false; }
    bool sendEffect(std::uint32_t, std::span<const std::uint8_t>) override { return false; }
    std::vector<input::JoystickInfo> joysticks() const override { return {{3, "Racing Wheel", true}}; }
    std::optional<input::RawJoystick> readJoystick(std::uint32_t id) override {
        if (id != 3) return std::nullopt;
        std::lock_guard lock(shared_->mutex);
        return shared_->raw;
    }
    bool setCentering(std::uint32_t, float strength) override {
        std::lock_guard lock(shared_->mutex);
        shared_->centering = strength;
        ++shared_->springCalls;
        return true;
    }

private:
    std::shared_ptr<Shared> shared_;
};

} // namespace

TEST(AppControllerSession, SteersWithAWheelOnceItsAxesAreShown) {
    test::MockConsole console({});
    auto wheel = std::make_shared<ScriptedWheel::Shared>();
    wheel->raw.axes = {0, 32767, 32767};
    const auto move = [&](auto change) {
        std::lock_guard lock(wheel->mutex);
        change(wheel->raw);
    };
    Harness h(console, "session_wheel.json", true, 60s, 0us, std::make_unique<ScriptedWheel>(wheel));
    ASSERT_TRUE(h.pair());
    h.app->chooseWheel("Racing Wheel");
    h.app->config().controller.wheel.enabled = true;
    h.app->configChanged();
    ASSERT_TRUE(h.runUntil([&] { return h.model.wheel.present; }));
    EXPECT_TRUE(h.model.wheel.force);

    // Turn the wheel to show the steering axis.
    h.app->captureWheel(static_cast<int>(input::WheelTarget::Steer));
    h.app->update();
    h.app->fillModel(h.model);
    EXPECT_EQ(h.model.wheel.capturing, static_cast<int>(input::WheelTarget::Steer));
    move([](input::RawJoystick& raw) { raw.axes[0] = -25000; });
    ASSERT_TRUE(h.runUntil([&] { return h.model.wheel.capturing == -1; }));
    EXPECT_EQ(h.app->config().controller.wheel.steerAxis, 0);
    move([](input::RawJoystick& raw) { raw.axes[0] = 0; });

    // Press the gas pedal all the way and let go.
    h.app->captureWheel(static_cast<int>(input::WheelTarget::Gas));
    h.app->update();
    move([](input::RawJoystick& raw) { raw.axes[2] = -32768; });
    h.app->update();
    move([](input::RawJoystick& raw) { raw.axes[2] = 32767; });
    ASSERT_TRUE(h.runUntil([&] { return h.model.wheel.capturing == -1; }));
    EXPECT_EQ(h.app->config().controller.wheel.gas.axis, 2);
    EXPECT_EQ(h.app->config().controller.wheel.gas.pressed, -32768);

    // A button for Cross.
    h.app->captureWheel(static_cast<int>(input::WheelTarget::Cross));
    h.app->update();
    move([](input::RawJoystick& raw) { raw.buttons = 1u << 4; });
    ASSERT_TRUE(h.runUntil([&] { return h.model.wheel.capturing == -1; }));
    EXPECT_EQ(h.app->config().controller.wheel.buttons[0], 4);
    move([](input::RawJoystick& raw) { raw.buttons = 0; });

    // No spring before the stream.
    h.app->update();
    EXPECT_EQ(wheel->centering, 0.0f);

    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    h.app->update();
    EXPECT_NEAR(wheel->centering, h.app->config().controller.wheel.centering, 0.01f);

    // Steering left with gas reaches the console as the left stick and R2.
    move([](input::RawJoystick& raw) {
        raw.axes[0] = -32768;
        raw.axes[2] = -32768;
        raw.buttons = 1u << 4;
    });
    h.app->update();
    const input::ControllerState sent = h.sessions->input();
    EXPECT_LT(sent.leftX, -30000);
    EXPECT_EQ(sent.r2, 255);
    EXPECT_TRUE(sent.pressed(input::Button::Cross));

    // After the stream the wheel lets go.
    h.app->disconnect();
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Idle; }));
    h.app->update();
    EXPECT_EQ(wheel->centering, 0.0f);
}

TEST(AppControllerSession, ConnectsAgainToApplyNewStreamSettings) {
    test::MockConsole console({});
    Harness h(console, "session_apply.json");
    ASSERT_TRUE(h.pair());
    h.app->selectProfile(kProfileSmooth30);
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    EXPECT_EQ(h.sessions->configs[0].stream.fps, 30);
    EXPECT_FALSE(h.model.session.settingsChanged);

    // Changed in the game's menu: the running stream keeps what it asked for,
    // and the model says that there is something to apply.
    h.app->config().stream.fps = 60;
    h.app->config().stream.bitrateKbps = 20'000;
    h.app->configChanged();
    h.app->update();
    h.app->fillModel(h.model);
    EXPECT_EQ(h.model.state, AppState::Streaming);
    EXPECT_TRUE(h.model.session.settingsChanged);
    EXPECT_EQ(h.sessions->configs.size(), 1u);

    // Applying ends the session and starts the next one right away, without a notice.
    h.app->applyStreamSettings();
    ASSERT_TRUE(h.runUntil([&] { return h.sessions->configs.size() == 2; }));
    EXPECT_EQ(h.sessions->stops, 1);
    EXPECT_EQ(h.sessions->configs[1].stream.fps, 60);
    EXPECT_EQ(h.sessions->configs[1].stream.bitrateKbps, 20'000);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    EXPECT_FALSE(h.model.session.settingsChanged);
    EXPECT_TRUE(h.model.notices.empty());
}

TEST(AppControllerSession, WaitsUntilTheConsoleLetsGoOfTheSessionBefore) {
    test::MockConsole console({});
    Harness h(console, "session_release.json");
    ASSERT_TRUE(h.pair());
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    // Connecting again right after our own session: the console still counts
    // that one as running and says "in use", twice, before it lets go.
    h.sessions->scripts.push_back({test::FakeSessionHub::quit(session::QuitReason::InUse)});
    h.sessions->scripts.push_back({test::FakeSessionHub::quit(session::QuitReason::InUse)});
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));

    h.app->applyStreamSettings();
    ASSERT_TRUE(h.runUntil([&] { return h.model.session.waitingForRelease; }));
    EXPECT_EQ(h.model.state, AppState::Reconnecting); // the screen with the spinner, not an error
    EXPECT_TRUE(h.model.notices.empty());

    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    EXPECT_EQ(h.sessions->configs.size(), 4u);
    EXPECT_FALSE(h.model.session.waitingForRelease);
    EXPECT_TRUE(h.model.notices.empty());

    // The same after disconnecting and connecting again from the start screen.
    h.app->disconnect();
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Idle; }));
    h.sessions->scripts.push_back({test::FakeSessionHub::quit(session::QuitReason::InUse)});
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    EXPECT_TRUE(h.model.notices.empty());
}

TEST(AppControllerSession, GivesUpWaitingWhenTheConsoleStaysTaken) {
    test::MockConsole console({});
    Harness h(console, "session_release_never.json");
    ASSERT_TRUE(h.pair());
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    // Somebody else took the console the moment we let go: it stays "in use".
    for (int i = 0; i < 200; ++i) h.sessions->scripts.push_back({test::FakeSessionHub::quit(session::QuitReason::InUse)});
    h.app->applyStreamSettings();
    ASSERT_TRUE(h.runUntil([&] { return h.model.session.waitingForRelease; }));
    ASSERT_TRUE(h.runUntil([&] { return h.hasNotice(Notice::Topic::SessionInUse); }, 5000ms));
    EXPECT_EQ(h.model.state, AppState::Error);
}

TEST(AppControllerSession, SendsTheMicrophoneOnlyWhileAskedToAndWhileAStreamRuns) {
    test::MockConsole console({});
    Harness h(console, "session_microphone.json");
    ASSERT_TRUE(h.pair());
    EXPECT_TRUE(h.model.microphoneAvailable);
    // Off unless the user switches it on: nothing is recorded, nothing is sent.
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    for (int i = 0; i < 5; ++i) h.app->update();
    EXPECT_EQ(h.microphone.starts, 0);
    EXPECT_EQ(h.sessions->micOpens, 0);
    EXPECT_FALSE(h.model.microphoneLive);

    // Switched on in the game: the console learns of the microphone, the mute is lifted, sound flows.
    h.app->config().audio.microphone = true;
    h.app->configChanged();
    ASSERT_TRUE(h.runUntil([&] { return h.model.microphoneLive; }));
    EXPECT_EQ(h.sessions->micOpens, 1);
    EXPECT_FALSE(h.sessions->micMuted);
    for (int i = 0; i < 10; ++i) ASSERT_TRUE(h.microphone.record());
    EXPECT_EQ(h.sessions->microphonePackets(), 10);
    EXPECT_EQ(h.sessions->micPacketSize, media::kMicPacketSize);

    // The device goes away and comes back: recording starts again by itself.
    h.microphone.unplug();
    ASSERT_TRUE(h.runUntil([&] { return h.microphone.starts >= 2 && h.model.microphoneLive; }));
    ASSERT_TRUE(h.microphone.record());

    // Switched off: recording ends and the console is told the microphone is muted.
    h.app->config().audio.microphone = false;
    h.app->configChanged();
    ASSERT_TRUE(h.runUntil([&] { return !h.model.microphoneLive; }));
    EXPECT_FALSE(h.microphone.record());
    EXPECT_TRUE(h.sessions->micMuted);

    // On again, then the stream ends: the microphone is silent before the session is gone.
    h.app->config().audio.microphone = true;
    h.app->configChanged();
    ASSERT_TRUE(h.runUntil([&] { return h.model.microphoneLive; }));
    const int stopsBefore = h.microphone.stops;
    h.app->disconnect();
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Idle; }));
    EXPECT_GT(h.microphone.stops, stopsBefore);
    EXPECT_FALSE(h.microphone.running());
    EXPECT_FALSE(h.microphone.record());
    EXPECT_FALSE(h.model.microphoneLive);
}

TEST(AppControllerSession, KeepsTryingAMicrophoneThatDoesNotOpen) {
    test::MockConsole console({});
    Harness h(console, "session_microphone_fails.json");
    ASSERT_TRUE(h.pair());
    h.microphone.failStart = true;
    h.app->config().audio.microphone = true;
    h.app->configChanged();
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    // Tried again and again, with a pause in between, and never claimed to be live.
    ASSERT_TRUE(h.runUntil([&] { return h.microphone.starts >= 3; }));
    EXPECT_FALSE(h.model.microphoneLive);
    // Once a microphone is there it is used.
    h.microphone.failStart = false;
    ASSERT_TRUE(h.runUntil([&] { return h.model.microphoneLive; }));
    EXPECT_TRUE(h.microphone.record());
}

TEST(AppControllerSession, UsesTheLayoutOfTheGameThatRuns) {
    test::MockConsole console({}); // says it runs the game with title id PPSA01325
    Harness h(console, "session_game_keys.json");
    ASSERT_TRUE(h.pair());
    input::KeyboardMouseSettings& kbm = h.app->config().controller.keyboardMouse;
    kbm.enabled = true;
    input::GameKeyLayout own;
    own.titleId = "PPSA01325";
    own.name = "Astro's Playroom";
    own.keys = kbm.keys;
    own.keys[static_cast<std::size_t>(input::PadControl::LeftStickUp)] = input::key::None;
    own.keys[static_cast<std::size_t>(input::PadControl::Cross)] = 'W';
    own.keys[static_cast<std::size_t>(input::PadControl::Square)] = input::key::None; // Cross had Shift before; free either way
    kbm.games.push_back(own);
    h.app->configChanged();
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    // The console names its game in the answers to the search; the layout follows.
    ASSERT_TRUE(h.runUntil([&] { return h.model.currentGameId == "PPSA01325"; }));
    EXPECT_EQ(h.model.currentGameName, "Astro's Playroom");
    // Answers that leave the game out keep it for the session (Discord showed no game otherwise).
    console.setNamesGame(false);
    const int before = console.searchesReceived();
    ASSERT_TRUE(h.runUntil([&] { return console.searchesReceived() >= before + 2; }));
    h.app->update();
    EXPECT_EQ(h.model.currentGameId, "PPSA01325");
    EXPECT_EQ(h.model.currentGameName, "Astro's Playroom");

    h.app->keyboardMouse().key('W', true);
    h.app->update();
    input::ControllerState sent = h.sessions->input();
    EXPECT_TRUE(sent.pressed(input::Button::Cross));
    EXPECT_EQ(sent.leftY, 0);
    h.app->keyboardMouse().releaseAll();

    // Without the game's layout the same key walks again.
    h.app->config().controller.keyboardMouse.games.clear();
    h.app->configChanged();
    h.app->keyboardMouse().key('W', true);
    h.app->update();
    sent = h.sessions->input();
    EXPECT_FALSE(sent.pressed(input::Button::Cross));
    EXPECT_EQ(sent.leftY, -32767);
    h.app->keyboardMouse().releaseAll();
}

TEST(AppControllerSession, PlaysWithKeysAndMouseInsteadOfAController) {
    test::MockConsole console({});
    Harness h(console, "session_keys.json"); // no controller is connected
    ASSERT_TRUE(h.pair());
    h.app->config().controller.keyboardMouse.enabled = true;
    h.app->configChanged();
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));

    // What the window passes on reaches the console as controller input.
    h.app->keyboardMouse().key('W', true);
    h.app->keyboardMouse().key(input::key::MouseLeft, true);
    h.app->update();
    input::ControllerState sent = h.sessions->input();
    EXPECT_EQ(sent.leftY, -32767);
    EXPECT_EQ(sent.r2, 255);
    h.app->keyboardMouse().releaseAll();
    h.app->update();
    sent = h.sessions->input();
    EXPECT_EQ(sent.leftY, 0);
    EXPECT_EQ(sent.r2, 0);

    // With keys and mouse set up, a missing controller is no fault.
    for (int i = 0; i < 120; ++i) {
        h.sessions->stats.videoFrames += 1;
        h.app->update();
        std::this_thread::sleep_for(15ms);
    }
    h.app->fillModel(h.model);
    EXPECT_TRUE(std::find(h.model.alerts.begin(), h.model.alerts.end(), StreamAlert::NoController) == h.model.alerts.end());

    // The settings ask for the next key: it arrives in the model, once.
    h.app->captureKey();
    h.app->fillModel(h.model);
    EXPECT_TRUE(h.model.capturingKey);
    EXPECT_EQ(h.model.capturedKey, 0);
    h.app->keyCaptured('E');
    h.app->fillModel(h.model);
    EXPECT_FALSE(h.model.capturingKey);
    EXPECT_EQ(h.model.capturedKey, 'E');
    h.app->endKeyCapture();
    h.app->fillModel(h.model);
    EXPECT_EQ(h.model.capturedKey, 0);
    // Esc ends the waiting without a key.
    h.app->captureKey();
    h.app->keyCaptured(0);
    h.app->fillModel(h.model);
    EXPECT_EQ(h.model.capturedKey, -1);
}

TEST(AppControllerSession, SaysOverThePictureWhatIsWrong) {
    test::MockConsole console({});
    // No controller is connected in this one, and the stream will stand still.
    Harness h(console, "session_notes.json", true, 60s);
    ASSERT_TRUE(h.pair());
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    EXPECT_TRUE(h.model.alerts.empty());

    const auto shows = [&](StreamAlert alert) {
        return std::find(h.model.alerts.begin(), h.model.alerts.end(), alert) != h.model.alerts.end();
    };
    // Pictures keep coming; the missing controller is said after a moment.
    ASSERT_TRUE(h.runUntil(
        [&] {
            h.sessions->stats.videoFrames += 1;
            return shows(StreamAlert::NoController);
        },
        4000ms));
    EXPECT_FALSE(shows(StreamAlert::ConnectionStalls));

    // Then no picture comes any more.
    ASSERT_TRUE(h.runUntil([&] { return shows(StreamAlert::ConnectionStalls); }, 6000ms));
    EXPECT_EQ(h.model.alerts.front(), StreamAlert::NoController);

    // With the session gone, so are the notes.
    h.app->disconnect();
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Idle; }));
    EXPECT_TRUE(h.model.alerts.empty());
}

TEST(AppControllerSession, AutomaticChoosesTheStreamAndLearnsFromAStall) {
    test::MockConsole console({});
    Harness h(console, "session_auto.json", true, 700ms);
    ASSERT_TRUE(h.pair());
    ASSERT_TRUE(isAutomatic(h.app->config()));
    h.app->config().stream.bitrateKbps = 6'000; // a value from before "automatic"; it plays no part
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    EXPECT_EQ(h.sessions->configs[0].stream.fps, 60);
    EXPECT_EQ(h.sessions->configs[0].stream.height, 1080);
    EXPECT_EQ(h.sessions->configs[0].stream.bitrateKbps, kAutoStartBitrateKbps);
    EXPECT_EQ(h.model.requestedStream.bitrateKbps, kAutoStartBitrateKbps);
    EXPECT_FALSE(h.model.session.settingsChanged);

    // No picture arrives: the next attempt asks for half, and that is remembered for this console.
    ASSERT_TRUE(h.runUntil([&] { return h.sessions->configs.size() == 2; }));
    EXPECT_EQ(h.sessions->configs[1].stream.bitrateKbps, 7'500);
    EXPECT_EQ(h.app->config().consoles[0].link.bitrateKbps, 7'500);
    EXPECT_EQ(h.app->config().consoles[0].link.ceilingKbps, 13'500);
    EXPECT_EQ(h.app->config().stream.bitrateKbps, 6'000);
}

TEST(AppControllerSession, KeepsAnotherBitrateForTheWayFromAway) {
    test::MockConsole console({});
    Harness h(console, "session_away.json", true, 700ms, 0us, nullptr, [](AppController::Options& options) {
        options.isAway = [](std::uint32_t) { return true; }; // as through a VPN
    });
    ASSERT_TRUE(h.pair());
    h.app->config().consoles[0].link = {20'000, 0}; // what home learned
    h.app->configChanged();
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    // Away starts carefully, whatever home learned, and the model says where it is.
    EXPECT_EQ(h.sessions->configs[0].stream.bitrateKbps, kAwayStartBitrateKbps);
    EXPECT_TRUE(h.model.session.away);
    EXPECT_EQ(h.model.requestedStream.bitrateKbps, kAwayStartBitrateKbps);
    // A stall away is learned for away; home keeps its rate.
    ASSERT_TRUE(h.runUntil([&] { return h.sessions->configs.size() == 2; }));
    EXPECT_EQ(h.sessions->configs[1].stream.bitrateKbps, 5'000);
    EXPECT_EQ(h.app->config().consoles[0].awayLink.bitrateKbps, 5'000);
    EXPECT_EQ(h.app->config().consoles[0].link.bitrateKbps, 20'000);
}

TEST(AppControllerSession, ReachesAConsoleKnownOnlyByItsSavedAddress) {
    // Through a VPN the search's broadcast never arrives; the console answers the direct question to
    // the address saved at pairing, and the session goes there.
    test::MockConsole console({});
    Harness h(console, "session_saved_address.json");
    ASSERT_TRUE(h.pair());
    ASSERT_FALSE(h.app->config().consoles[0].lastAddress.empty());
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    EXPECT_EQ(h.sessions->configs[0].host, h.app->config().consoles[0].lastAddress);
    EXPECT_FALSE(h.model.session.away); // without a check, every console counts as at home
}

TEST(AppControllerSession, RestartsAStreamThatStandsStillAtHalfTheBitrate) {
    test::MockConsole console({});
    // The timeout is real time. It has to be long against how late a sleep can
    // come back on a busy machine: the self-hosted runner builds with low
    // priority while its owner plays, and 150 ms was not enough there.
    Harness h(console, "session_stall.json", true, 700ms);
    ASSERT_TRUE(h.pair());
    h.app->config().activeProfile = std::string(kProfileCustom);
    h.app->config().stream.bitrateKbps = 40'000;
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    h.app->connect(Harness::kHostId);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    EXPECT_EQ(h.sessions->configs[0].stream.bitrateKbps, 40'000);

    // Pictures arrive for a while: nothing happens.
    for (int i = 0; i < 20; ++i) {
        h.sessions->stats.videoFrames += 3;
        h.app->update();
        std::this_thread::sleep_for(15ms);
    }
    EXPECT_EQ(h.sessions->configs.size(), 1u);

    // Then they stop. The session is ended and started again at half the rate.
    ASSERT_TRUE(h.runUntil([&] { return h.sessions->configs.size() == 2; }));
    EXPECT_EQ(h.sessions->configs[1].stream.bitrateKbps, 20'000);
    EXPECT_EQ(h.sessions->stops, 1);
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Streaming; }));
    ASSERT_TRUE(h.hasNotice(Notice::Topic::BitrateLowered));
    EXPECT_EQ(h.model.notices[0].detail, "20");
    EXPECT_EQ(h.app->config().stream.bitrateKbps, 40'000); // the setting itself is the user's

    // It keeps halving down to the floor, then gives up and says so.
    for (int i = 0; i < 4; ++i) h.sessions->scripts.push_back({test::FakeSessionHub::connected()});
    ASSERT_TRUE(h.runUntil([&] { return h.model.state == AppState::Error; }, 15000ms));
    ASSERT_EQ(h.sessions->configs.size(), 4u);
    EXPECT_EQ(h.sessions->configs[2].stream.bitrateKbps, 10'000);
    EXPECT_EQ(h.sessions->configs[3].stream.bitrateKbps, 5'000);
    EXPECT_TRUE(h.hasNotice(Notice::Topic::SessionLost));
}
