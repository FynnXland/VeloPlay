// veloplay-cli: discovery, wake-up, pairing and a test stream without the graphical client.
// Meant for checking a setup and for bug reports: its output shows what the
// console answered at each step.

#ifdef VELOPLAY_LINUX_PLATFORM
#include "linux/LinuxServices.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include "app/Config.hpp"
#include "net/Socket.hpp"
#include "protocol/ConnectionProbe.hpp"
#include "protocol/Discovery.hpp"
#include "protocol/DiscoveryService.hpp"
#include "protocol/RegistrationClient.hpp"
#include "session/ISession.hpp"
#include "update/HttpClient.hpp"
#include "update/Release.hpp"
#include "update/Updater.hpp"
#include "update/Version.hpp"

using namespace veloplay;
using namespace std::chrono_literals;

namespace {

const char kUsage[] =
    "veloplay-cli " VELOPLAY_VERSION "\n"
    "\n"
    "Usage:\n"
    "  veloplay-cli discover [--host <ip>] [--seconds <n>]\n"
    "      Lists the consoles that answer on the local network.\n"
    "\n"
    "  veloplay-cli ping --host <ip> [--seconds <n>]\n"
    "      Measures how fast and how evenly a console answers. Shows whether the\n"
    "      network between PC and console is steady enough for streaming.\n"
    "\n"
    "  veloplay-cli check-update\n"
    "  veloplay-cli update-folder <folder>   install the latest release over the VeloPlay in <folder>\n"
    "      Asks GitHub for the latest release (Windows only).\n"
    "\n"
    "  veloplay-cli wake --host <ip> --regist-key <key>\n"
    "      Wakes a console from rest mode. The key is printed by \"pair\".\n"
    "\n"
    "  veloplay-cli pair --host <ip> --account-id <id> --pin <pin> [--show-keys]\n"
    "      Pairs with a console. Open Settings > System > Remote Play > Link Device\n"
    "      on the PS5 first and use the PIN it shows.\n"
    "      The graphical client stores its own pairing; this command only tests it.\n"
    "\n"
    "  veloplay-cli stream [--host <ip>] [--seconds <n>] [--out <file>]\n"
    "                      [--fps 30|60] [--bitrate <kbit/s>] [--h264]\n"
    "                      [--regist-key <key> --rp-key <hex>]\n"
    "      Connects to a console and reports what arrives. With --out the video is\n"
    "      written to a file that VLC or ffplay can open. On Windows the keys come\n"
    "      from the console paired in VeloPlay; elsewhere pass them as printed by\n"
    "      \"pair --show-keys\".\n";

std::string option(const std::vector<std::string>& args, const std::string& name) {
    const auto it = std::find(args.begin(), args.end(), name);
    return (it != args.end() && it + 1 != args.end()) ? *(it + 1) : std::string{};
}

bool flag(const std::vector<std::string>& args, const std::string& name) {
    return std::find(args.begin(), args.end(), name) != args.end();
}

const char* stateName(protocol::HostState state) {
    switch (state) {
        case protocol::HostState::Ready: return "ready";
        case protocol::HostState::Standby: return "rest mode";
        case protocol::HostState::Unknown: break;
    }
    return "unknown";
}

int discover(const std::vector<std::string>& args) {
    // Long enough for the direct search that starts when the broadcast finds nothing.
    int seconds = 8;
    if (const std::string value = option(args, "--seconds"); !value.empty()) seconds = std::max(1, std::atoi(value.c_str()));

    protocol::DiscoveryService service;
    if (const std::string host = option(args, "--host"); !host.empty()) {
        const auto ip = net::parseIpv4(host);
        if (!ip) {
            std::fprintf(stderr, "\"%s\" is not an IPv4 address.\n", host.c_str());
            return 2;
        }
        service.setDirectTargets({*ip});
    }

    // The steps of the search are what this command is for.
    spdlog::set_level(spdlog::level::info);
    std::printf("Local networks:");
    for (const net::LocalSubnet& subnet : net::localSubnets()) {
        std::printf(" %s/%u", net::formatIpv4(subnet.address).c_str(), subnet.prefix);
    }
    std::printf("\nBroadcast addresses:");
    for (std::uint32_t address : net::broadcastAddresses()) std::printf(" %s", net::formatIpv4(address).c_str());
    std::printf("\nSearching for %d s ...\n", seconds);

    if (const auto started = service.start(); !started) {
        std::fprintf(stderr, "Discovery could not start: %s\n", started.error().message.c_str());
        return 1;
    }
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    const auto hosts = service.hosts();
    service.stop();

    if (hosts.empty()) {
        std::printf("No console answered.\n"
                    "Check that the PS5 is on or in rest mode, that it is on this network and that Remote Play\n"
                    "is enabled (Settings > System > Remote Play). If the console is on another subnet, pass\n"
                    "its address with --host.\n");
        return 1;
    }
    for (const auto& host : hosts) {
        std::printf("\n%s\n", host.info.hostName.empty() ? "(no name)" : host.info.hostName.c_str());
        std::printf("  address        %s\n", net::formatIpv4(host.address).c_str());
        std::printf("  state          %s (status %d)\n", stateName(host.info.state), host.info.statusCode);
        std::printf("  type           %s\n", host.info.hostType.c_str());
        std::printf("  host id        %s\n", host.info.hostId.c_str());
        std::printf("  system version %s\n", host.info.systemVersion.c_str());
        std::printf("  protocol       %s\n", host.info.protocolVersion.c_str());
        if (!host.info.runningAppName.empty()) {
            std::printf("  running        %s (%s)\n", host.info.runningAppName.c_str(), host.info.runningAppTitleId.c_str());
        }
    }
    return 0;
}

int ping(const std::vector<std::string>& args) {
    const auto ip = net::parseIpv4(option(args, "--host"));
    if (!ip) {
        std::fprintf(stderr, "ping needs --host <ip>.\n");
        return 2;
    }
    int seconds = 15;
    if (const std::string value = option(args, "--seconds"); !value.empty()) seconds = std::max(5, std::atoi(value.c_str()));

    protocol::ConnectionProbe probe;
    (void)probe.start(*ip);
    std::printf("Measuring the link to %s for %d s ...\n", net::formatIpv4(*ip).c_str(), seconds);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    probe.stop();

    const protocol::LatencySnapshot s = probe.snapshot();
    if (s.answered() == 0) {
        std::printf("No answer to %d probes. Is the address right and the console on or in rest mode?\n", s.sent);
        return 1;
    }
    const char* rating = "unknown";
    switch (protocol::rateLink(s)) {
        case protocol::LinkQuality::Excellent: rating = "excellent, as steady as a cable"; break;
        case protocol::LinkQuality::Good: rating = "good"; break;
        case protocol::LinkQuality::Fair: rating = "fair, expect occasional stutter"; break;
        case protocol::LinkQuality::Poor: rating = "poor, expect stutter and input delay"; break;
        case protocol::LinkQuality::Unknown: break;
    }
    const protocol::StreamAdvice advice = protocol::adviceFor(protocol::rateLink(s));
    std::printf("  probes         %d sent, %d lost (%.1f %%)\n", s.sent, s.lost, static_cast<double>(s.lossPercent()));
    std::printf("  response time  %.1f ms average, %.1f ms best, %.1f ms worst, %.1f ms for 95 %%\n",
                static_cast<double>(s.average), static_cast<double>(s.minimum), static_cast<double>(s.maximum),
                static_cast<double>(s.p95));
    std::printf("  variation      %.1f ms\n", static_cast<double>(s.jitter));
    std::printf("  rating         %s\n", rating);
    std::printf("  suggestion     %d Mbit/s, %s network buffer\n", advice.bitrateKbps / 1000,
                advice.stableBuffer ? "stable" : "low-latency");
    return 0;
}

int checkUpdate() {
#if defined(_WIN32) || defined(VELOPLAY_LINUX_PLATFORM)
#ifdef _WIN32
    const auto http = update::createWinHttpClient("VeloPlay/" VELOPLAY_VERSION);
#else
    const auto http = linuxos::createCurlHttpClient("VeloPlay/" VELOPLAY_VERSION);
#endif
    const auto response = http->get(update::latestReleaseApiUrl(), 1024 * 1024, {}, {});
    if (!response) {
        std::printf("No release information: %s\n", response.error().message.c_str());
        std::printf("HTTP 404 means that nothing is published yet or that the repository is private.\n");
        return 1;
    }
    const auto found = update::findRelease(asText(*response));
    if (!found) {
        std::printf("The latest release cannot be used: %s\n", found.error().message.c_str());
        return 1;
    }
    // The Windows file comes first; the one for this system can follow a few minutes later.
    if (!*found) {
        std::printf("Installed %s: the latest release has no download for this system yet.\n", VELOPLAY_VERSION);
        return 0;
    }
    const auto& release = *found;
    const auto current = update::Version::parse(VELOPLAY_VERSION).value_or(update::Version{});
    std::printf("Installed %s, latest %s: %s\n", current.toString().c_str(), release->version.toString().c_str(),
                release->version > current ? "an update is available" : "up to date");
    return 0;
#else
    std::printf("This build has no HTTPS client.\n");
    return 1;
#endif
}

int wake(const std::vector<std::string>& args) {
    const auto ip = net::parseIpv4(option(args, "--host"));
    const auto credential = protocol::wakeCredential(option(args, "--regist-key"));
    if (!ip || !credential) {
        std::fprintf(stderr, "wake needs --host <ip> and --regist-key <key>.\n");
        return 2;
    }

    protocol::DiscoveryService service;
    service.setDirectTargets({*ip});
    if (const auto started = service.start(); !started) {
        std::fprintf(stderr, "Discovery could not start: %s\n", started.error().message.c_str());
        return 1;
    }
    if (const auto sent = service.wake(*ip, *credential); !sent) {
        std::fprintf(stderr, "The wake-up packet could not be sent: %s\n", sent.error().message.c_str());
        return 1;
    }
    std::printf("Wake-up sent to %s. Waiting for the console (up to 60 s) ...\n", net::formatIpv4(*ip).c_str());

    for (int second = 0; second < 60; ++second) {
        std::this_thread::sleep_for(1s);
        for (const auto& host : service.hosts()) {
            if (host.address == *ip && host.info.state == protocol::HostState::Ready) {
                std::printf("%s is ready after %d s.\n", host.info.hostName.c_str(), second + 1);
                return 0;
            }
        }
        if (second % 5 == 4) (void)service.wake(*ip, *credential);
    }
    std::printf("The console did not become ready. On the PS5, check Settings > System > Power Saving >\n"
                "Features Available in Rest Mode: \"Stay Connected to the Internet\" and \"Enable Turning On\n"
                "PS5 from Network\" have to be on.\n");
    return 1;
}

int pair(const std::vector<std::string>& args) {
    const auto ip = net::parseIpv4(option(args, "--host"));
    const auto account = protocol::PsnAccountId::parse(option(args, "--account-id"));
    const auto pin = protocol::parsePin(option(args, "--pin"));
    if (!ip) std::fprintf(stderr, "--host needs an IPv4 address such as 192.168.1.20.\n");
    if (!account) std::fprintf(stderr, "--account-id needs the base64 form (12 characters) or the decimal number.\n");
    if (!pin) std::fprintf(stderr, "--pin needs the eight digits shown by the PS5.\n");
    if (!ip || !account || !pin) return 2;

    protocol::RegistrationParams params;
    params.address = *ip;
    params.account = *account;
    params.pin = *pin;
    std::printf("Pairing with %s as account %s ...\n", net::formatIpv4(*ip).c_str(), account->toBase64().c_str());

    const auto host = protocol::registerWithConsole(params);
    if (!host) {
        std::fprintf(stderr, "Pairing failed (%s): %s\n", toString(host.error().code), host.error().message.c_str());
        return 1;
    }
    std::printf("Paired with \"%s\".\n", host->nickname.c_str());
    std::printf("  host id     %s\n", host->hostId().c_str());
    std::printf("  key type    %u\n", host->rpKeyType);
    if (flag(args, "--show-keys")) {
        std::printf("  regist key  %s\n", host->registKey.c_str());
        std::printf("  rp key      %s\n", toHex(host->rpKey).c_str());
        std::printf("Keep these values private: they give access to your console.\n");
    } else {
        std::printf("  keys        received (pass --show-keys to print them)\n");
    }
    return 0;
}

/// Writes the video to a file and keeps what the console said about the audio.
class DumpSink final : public session::IMediaSink {
public:
    explicit DumpSink(const std::string& path) {
        if (!path.empty()) file_.open(path, std::ios::binary | std::ios::trunc);
    }

    [[nodiscard]] bool fileOk() const { return file_.is_open() && file_.good(); }

    bool onVideo(std::span<const std::uint8_t> accessUnit, int, bool) override {
        {
            // How evenly frames arrive: the longest pause, and how many came
            // within 20 ms of each other (a burst after a pause).
            std::lock_guard lock(mutex_);
            const auto now = std::chrono::steady_clock::now();
            if (lastFrame_ != std::chrono::steady_clock::time_point{}) {
                const float gap = std::chrono::duration<float, std::milli>(now - lastFrame_).count();
                longestGapMs_ = std::max(longestGapMs_, gap);
                burst_ = gap < 20.0f ? burst_ + 1 : 1;
                largestBurst_ = std::max(largestBurst_, burst_);
            }
            lastFrame_ = now;
        }
        if (file_.is_open()) {
            file_.write(reinterpret_cast<const char*>(accessUnit.data()), static_cast<std::streamsize>(accessUnit.size()));
        }
        return true;
    }
    void onAudioFormat(const session::AudioFormat& format) override {
        std::lock_guard lock(mutex_);
        audio_ = format;
    }
    void onAudio(std::span<const std::uint8_t>) override {}

    /// Longest pause between frames and largest burst since the last call.
    std::pair<float, int> takeArrivalPattern() {
        std::lock_guard lock(mutex_);
        const std::pair<float, int> out{longestGapMs_, largestBurst_};
        longestGapMs_ = 0.0f;
        largestBurst_ = 1;
        return out;
    }

    std::optional<session::AudioFormat> audioFormat() {
        std::lock_guard lock(mutex_);
        return audio_;
    }

private:
    std::ofstream file_;
    std::mutex mutex_;
    std::optional<session::AudioFormat> audio_;
    std::chrono::steady_clock::time_point lastFrame_{};
    float longestGapMs_ = 0.0f;
    int burst_ = 1;
    int largestBurst_ = 1;
};

const char* quitText(session::QuitReason reason) {
    switch (reason) {
        case session::QuitReason::Stopped: return "ended by this program";
        case session::QuitReason::ConsoleShutDown: return "the console went to rest mode or was switched off";
        case session::QuitReason::ConnectionRefused:
            return "the console refused the connection (rest mode, or Remote Play is switched off)";
        case session::QuitReason::InUse: return "another Remote Play device is connected";
        case session::QuitReason::ConsoleCrashed: return "Remote Play crashed on the console";
        case session::QuitReason::VersionMismatch: return "the console uses a Remote Play version this client does not know";
        case session::QuitReason::SetupFailed: return "the handshake failed (wrong keys, or the network dropped packets)";
        case session::QuitReason::ConnectionLost: return "the stream broke";
    }
    return "unknown";
}

int stream(const std::vector<std::string>& args) {
    session::SessionConfig config;
    config.host = option(args, "--host");
    config.registKey = option(args, "--regist-key");
    const std::string rpKeyHex = option(args, "--rp-key");
    std::string consoleName = config.host;

    if (!rpKeyHex.empty()) {
        const auto key = fromHex(rpKeyHex);
        if (!key || key->size() != config.rpKey.size()) {
            std::fprintf(stderr, "--rp-key needs 32 hex digits.\n");
            return 2;
        }
        std::copy(key->begin(), key->end(), config.rpKey.begin());
    }

#ifdef _WIN32
    if (config.registKey.empty() && rpKeyHex.empty()) {
        // Use what the graphical client stored when it paired.
        const auto protector = createDpapiProtector();
        ConfigStore store(defaultConfigDirectory() / "config.json", *protector);
        const auto loaded = store.load();
        if (!loaded) {
            std::fprintf(stderr, "The settings of VeloPlay could not be read: %s\n", loaded.error().message.c_str());
            return 1;
        }
        const RegisteredConsole* chosen = nullptr;
        for (const RegisteredConsole& console : loaded->consoles) {
            if (!console.credentialsValid) continue;
            if (config.host.empty() || console.lastAddress == config.host) {
                chosen = &console;
                break;
            }
        }
        if (!chosen && loaded->consoles.size() == 1 && loaded->consoles[0].credentialsValid) chosen = &loaded->consoles[0];
        if (!chosen) {
            std::fprintf(stderr, "No paired console found. Pair in VeloPlay first, or pass --regist-key and --rp-key.\n");
            return 1;
        }
        config.registKey = chosen->registKey;
        config.rpKey = chosen->rpKey;
        if (config.host.empty()) config.host = chosen->lastAddress;
        consoleName = chosen->name;
        config.stream = loaded->stream;
    }
#endif

    if (config.host.empty() || config.registKey.empty() || (rpKeyHex.empty() && config.rpKey == crypto::Key128{})) {
        std::fprintf(stderr, "stream needs --host <ip>, --regist-key <key> and --rp-key <hex>.\n");
        return 2;
    }

    int seconds = 10;
    if (const std::string value = option(args, "--seconds"); !value.empty()) seconds = std::max(1, std::atoi(value.c_str()));
    if (const std::string value = option(args, "--fps"); !value.empty()) config.stream.fps = std::atoi(value.c_str());
    if (const std::string value = option(args, "--bitrate"); !value.empty()) config.stream.bitrateKbps = std::atoi(value.c_str());
    if (flag(args, "--h264")) config.stream.codec = VideoCodec::H264;
    config.stream.hdr = false; // an HDR stream needs tone mapping to look right in a player
    sanitize(config.stream);

    const std::string outPath = option(args, "--out");
    DumpSink sink(outPath);
    if (!outPath.empty() && !sink.fileOk()) {
        std::fprintf(stderr, "Cannot write to %s.\n", outPath.c_str());
        return 1;
    }

    spdlog::set_level(spdlog::level::info); // the handshake steps are what this command is for
    std::printf("Connecting to %s: %dx%d, %d fps, %s, %d kbit/s ...\n", consoleName.c_str(), config.stream.width,
                config.stream.height, config.stream.fps, config.stream.codec == VideoCodec::Hevc ? "HEVC" : "H.264",
                config.stream.bitrateKbps);

    auto session = session::chiakiSessionFactory()();
    const auto started = session->start(config, &sink);
    if (!started) {
        std::fprintf(stderr, "The session could not be started: %s\n", started.error().message.c_str());
        return 1;
    }

    using Clock = session::Clock;
    bool connected = false;
    bool stopRequested = false;
    Clock::time_point connectedAt{};
    Clock::time_point nextReport{};
    const Clock::time_point giveUp = Clock::now() + 60s;
    session::SessionStats last{};
    int exitCode = 1;

    while (true) {
        const auto now = Clock::now();
        bool quit = false;
        for (const session::SessionEvent& event : session->pollEvents()) {
            if (event.type == session::SessionEvent::Type::Connected) {
                connected = true;
                connectedAt = now;
                nextReport = now + 1s;
                std::printf("Connected. Receiving for %d s ...\n", seconds);
            } else if (event.type == session::SessionEvent::Type::LoginPinRequested) {
                std::printf("The console asks for the login passcode of your user. Connect with VeloPlay to enter it.\n");
                session->stop();
                stopRequested = true;
            } else if (event.type == session::SessionEvent::Type::Quit) {
                if (event.reason != session::QuitReason::Stopped || !connected) {
                    std::printf("Session ended: %s (%s)\n", quitText(event.reason), event.detail.c_str());
                }
                if (event.reason == session::QuitReason::Stopped && connected) exitCode = 0;
                quit = true;
            }
        }
        if (quit) break;

        if (connected && !stopRequested) {
            last = session->stats(now);
            if (now >= nextReport) {
                nextReport += 1s;
                const auto [gapMs, burst] = sink.takeArrivalPattern();
                std::printf("  %5.1f fps  %5.1f Mbit/s  loss %4.1f %%  frames %llu (lost %llu)  audio packets %llu"
                            "  longest pause %3.0f ms  burst %d\n",
                            last.streamFps, last.bitrateMbps, last.packetLossPercent,
                            static_cast<unsigned long long>(last.videoFrames),
                            static_cast<unsigned long long>(last.framesLost),
                            static_cast<unsigned long long>(last.audioPackets), static_cast<double>(gapMs), burst);
                std::fflush(stdout);
            }
            if (now - connectedAt >= std::chrono::seconds(seconds)) {
                session->stop();
                stopRequested = true;
            }
        } else if (!connected && !stopRequested && now >= giveUp) {
            std::printf("No connection after 60 s, giving up.\n");
            session->stop();
            stopRequested = true;
        }
        std::this_thread::sleep_for(20ms);
    }
    session.reset();

    if (connected) {
        std::printf("\nReceived %llu video frames (%.1f MB, %llu lost) and %llu audio packets. Response time at setup: %.1f ms.\n",
                    static_cast<unsigned long long>(last.videoFrames), static_cast<double>(last.videoBytes) / 1e6,
                    static_cast<unsigned long long>(last.framesLost), static_cast<unsigned long long>(last.audioPackets),
                    last.roundTripMs);
        if (const auto audio = sink.audioFormat()) {
            std::printf("Audio: %u channels, %u Hz, %u bit.\n", audio->channels, audio->sampleRate, audio->bitsPerSample);
        }
        if (!outPath.empty()) {
            std::printf("Video written to %s. Play it with: ffplay %s\n", outPath.c_str(), outPath.c_str());
        }
        if (last.videoFrames == 0) {
            std::printf("No video frame arrived. Please report this with the lines above.\n");
            exitCode = 1;
        }
    }
    return exitCode;
}

} // namespace

/// The updater as the program runs it, pointed at a folder: asks GitHub, downloads, checks the digest, unpacks
/// and swaps the files in. For trying the whole way of an update without the window, and for a VeloPlay whose
/// own window does not start any more.
int updateFolder(const std::vector<std::string>& args) {
#if defined(_WIN32) || defined(VELOPLAY_LINUX_PLATFORM)
    if (args.size() < 2) {
        std::fprintf(stderr, "update-folder needs the folder VeloPlay lies in.\n");
        return 2;
    }
#ifdef _WIN32
    const auto http = update::createWinHttpClient("VeloPlay/" VELOPLAY_VERSION);
#else
    const auto http = linuxos::createCurlHttpClient("VeloPlay/" VELOPLAY_VERSION);
#endif
    update::Updater::Options options;
    options.current = update::Version::parse(VELOPLAY_VERSION).value_or(update::Version{});
    options.installDir = args[1];
    options.workDir = std::filesystem::path(args[1]) / "update";
    options.http = http.get();
    update::Updater updater(std::move(options));
    using Phase = update::UpdateStatus::Phase;
    const auto wait = [&](std::initializer_list<Phase> busy) {
        for (;;) {
            const update::UpdateStatus status = updater.status();
            if (std::find(busy.begin(), busy.end(), status.phase) == busy.end()) return status;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };
    updater.check();
    update::UpdateStatus status = wait({Phase::Idle, Phase::Checking});
    if (status.phase == Phase::UpToDate) {
        std::printf("Installed %s: nothing newer for this system.\n", status.current.toString().c_str());
        return 0;
    }
    if (status.phase != Phase::Available) {
        std::printf("The check failed: %s\n", status.detail.c_str());
        return 1;
    }
    std::printf("Installing %s over %s in %s\n", status.latest.toString().c_str(), status.current.toString().c_str(),
                args[1].c_str());
    updater.install();
    status = wait({Phase::Available, Phase::Downloading, Phase::Installing});
    if (status.phase != Phase::RestartNeeded) {
        std::printf("The update failed: %s\n", status.detail.c_str());
        return 1;
    }
    std::printf("Done: %s is in place.\n", status.latest.toString().c_str());
    return 0;
#else
    (void)args;
    std::printf("This build has no HTTPS client.\n");
    return 1;
#endif
}

int main(int argc, char** argv) {
    spdlog::set_level(spdlog::level::warn);
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        std::fputs(kUsage, stdout);
        return args.empty() ? 2 : 0;
    }
    if (args[0] == "discover") return discover(args);
    if (args[0] == "ping") return ping(args);
    if (args[0] == "check-update") return checkUpdate();
    if (args[0] == "update-folder") return updateFolder(args);
    if (args[0] == "wake") return wake(args);
    if (args[0] == "pair") return pair(args);
    if (args[0] == "stream") return stream(args);
    std::fprintf(stderr, "Unknown command \"%s\".\n\n%s", args[0].c_str(), kUsage);
    return 2;
}
