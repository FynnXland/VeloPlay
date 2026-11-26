#include "linux/LinuxApplication.hpp"

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl3.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <GL/glcorearb.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <utility>

#include "app/DiscordPresence.hpp"
#include "app/Hardware.hpp"
#include "app/HelpLinks.hpp"
#include "input/SdlControllerBackend.hpp"
#include "linux/DesktopLinux.hpp"
#include "linux/InstallerLinux.hpp"
#include "linux/KeyMap.hpp"
#include "linux/LinuxServices.hpp"
#include "linux/V4l2Capture.hpp"
#include "media/Snapshot.hpp"
#include "net/Socket.hpp"
#include "protocol/PsnAccount.hpp"
#include "session/ISession.hpp"
#include "ui/Strings.hpp"
#include "update/Package.hpp"
#include "util/LogFiles.hpp"

extern char** environ;

namespace veloplay::linuxos {

namespace {

// Over Bluetooth the kernel gives the DualSense's touchpad an input device of its own, and the desktop takes it
// as a mouse: a press on it clicks on the desktop. Telling libinput to leave it alone keeps it away from the
// desktop; SDL reads the controller through hidraw and still gets the touchpad.
constexpr const char* kTouchpadRuleFile = "/etc/udev/rules.d/72-veloplay-controller-touchpad.rules";
constexpr const char* kTouchpadRule =
    "ACTION==\"add|change\", KERNEL==\"event*\", ATTRS{name}==\"*Wireless Controller Touchpad\", "
    "ENV{LIBINPUT_IGNORE_DEVICE}=\"1\"";

/// Writes the rule as root through pkexec, which asks for the password in a window of the desktop. Blocking.
bool writeTouchpadRule() {
    const std::string script = std::string("printf '%s\\n' '") + kTouchpadRule + "' > " + kTouchpadRuleFile +
                               " && udevadm control --reload && udevadm trigger --subsystem-match=input";
    const char* argv[] = {"pkexec", "/bin/sh", "-c", script.c_str(), nullptr};
    pid_t child = 0;
    if (posix_spawnp(&child, "pkexec", nullptr, nullptr, const_cast<char* const*>(argv), environ) != 0) return false;
    int status = 0;
    if (waitpid(child, &status, 0) != child) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

constexpr int kDefaultWidth = 1100;
constexpr int kDefaultHeight = 720;
constexpr int kMinWidth = 820;
constexpr int kMinHeight = 560;
constexpr int kLogsKept = 3;

void setUpLogging(const std::filesystem::path& directory) {
    std::vector<spdlog::sink_ptr> sinks;
    // Started from a terminal, the log shows there as well; that is how a Linux user looks first.
    if (isatty(STDERR_FILENO)) sinks.push_back(std::make_shared<spdlog::sinks::stderr_color_sink_mt>());
    try {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        const std::filesystem::path file = rotateLogFiles(directory, "veloplay", kLogsKept);
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(file.string(), true));
    } catch (const spdlog::spdlog_ex&) {
        // No log file, for example on a read-only home. The client still works.
    }
    auto logger = std::make_shared<spdlog::logger>("veloplay", sinks.begin(), sinks.end());
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(std::move(logger));
    spdlog::flush_every(std::chrono::seconds(1));
    spdlog::info("VeloPlay {} starting on Linux", VELOPLAY_VERSION);
}

std::filesystem::path executablePath() {
    std::error_code ec;
    std::filesystem::path path = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path{} : path;
}

/// "de" when the desktop speaks German. The first of these that is set decides, as for any program.
std::string desktopLanguage() {
    for (const char* name : {"LC_ALL", "LC_MESSAGES", "LANGUAGE", "LANG"}) {
        const char* value = std::getenv(name);
        if (!value || !*value) continue;
        return std::string_view(value).starts_with("de") ? "de" : "en";
    }
    return "en";
}

/// The first font file of the list that is there. Distributions put the same fonts in different places.
std::string firstFont(std::initializer_list<const char*> candidates) {
    std::error_code ec;
    for (const char* candidate : candidates) {
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

/// What fontconfig names for a pattern, if fontconfig is there. Only TrueType and OpenType files are taken.
std::string fontconfigFont(const char* pattern) {
    const std::string command = std::string("fc-match -f '%{file}' '") + pattern + "' 2>/dev/null";
    std::FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) return {};
    char buffer[1024] = {};
    const std::size_t read = std::fread(buffer, 1, sizeof(buffer) - 1, pipe);
    pclose(pipe);
    std::string file(buffer, read);
    if (!(file.ends_with(".ttf") || file.ends_with(".otf"))) return {};
    std::error_code ec;
    return std::filesystem::is_regular_file(file, ec) ? file : std::string{};
}

std::string localClock() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char text[8] = {};
    std::snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
    return text;
}

/// OpenGL functions for the renderer, through SDL, which knows whether GLX or EGL made the context.
void* glFunction(const char* name) { return reinterpret_cast<void*>(SDL_GL_GetProcAddress(name)); }

std::tm localNow() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    return local;
}

} // namespace

LinuxApplication::LinuxApplication(int argc, char** argv) {
    for (int i = 0; i < argc; ++i) arguments_.emplace_back(argv[i]);
    executable_ = executablePath();
}

LinuxApplication::~LinuxApplication() {
    captureScan_.request_stop();
    if (captureScan_.joinable()) captureScan_.join();
    ui_.reset();
    controller_.reset(); // stops background threads before the window goes away
    // Sound threads of SDL end before SDL itself does.
    if (microphone_) microphone_->stop();
    if (audioOutput_) audioOutput_->stop();
    if (hapticsOutput_) hapticsOutput_->stop();
    // What lives on the card goes while its context is still there.
    if (gl_) {
        SDL_GL_MakeCurrent(window_, gl_);
        grabber_.release();
        recordStream_.reset();
        videoRenderer_.reset();
    }
    if (imguiReady_) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext();
    }
    if (gl_) SDL_GL_DestroyContext(gl_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

std::optional<int> LinuxApplication::runCommand() {
    bool install = false;
    bool remove = false;
    bool quiet = false;
    bool about = false;
    for (std::size_t i = 1; i < arguments_.size(); ++i) {
        const std::string& argument = arguments_[i];
        if (argument == "--install") install = true;
        if (argument == "--uninstall") remove = true;
        if (argument == "--quiet") quiet = true;
        if (argument == "--about" || argument == "--version") about = true;
        if (argument == "--help") {
            std::cout << "VeloPlay " << VELOPLAY_VERSION << "\n"
                      << "  --install      install for this user (menu entry), without a window\n"
                      << "  --uninstall    remove the installed program (add --quiet: ask nothing)\n"
                      << "  --about        version and what this build can use\n"
                      << "  --through-sony reach consoles through Sony's servers even at home\n";
            return 0;
        }
    }
    if (about) {
        // What SDL was built with: video and sound reach the desktop only through these.
        std::cout << "VeloPlay " << VELOPLAY_VERSION << " for Linux, SDL " << SDL_MAJOR_VERSION << '.' << SDL_MINOR_VERSION << '.'
                  << SDL_MICRO_VERSION << "\nVideo:";
        for (int i = 0; i < SDL_GetNumVideoDrivers(); ++i) std::cout << ' ' << SDL_GetVideoDriver(i);
        std::cout << "\nSound:";
        for (int i = 0; i < SDL_GetNumAudioDrivers(); ++i) std::cout << ' ' << SDL_GetAudioDriver(i);
        std::cout << "\nThrough Sony's servers: " << (session::psnAvailable() ? "yes" : "no")
                  << "\nCertificates: " << (caBundle().empty() ? "none found" : caBundle()) << '\n';
        return 0;
    }
    if (remove) {
        // There is no list of installed apps on Linux: this is the way to remove VeloPlay.
        const bool german = desktopLanguage() == "de";
        const ui::Strings& t = ui::strings(german ? ui::Language::German : ui::Language::English);
        return uninstall(executable_, quiet, [&](UninstallOutcome outcome) {
            const char* text = outcome == UninstallOutcome::StillRunning ? t.uninstallRunning : t.uninstallDone;
            if (!SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "VeloPlay", text, nullptr)) std::cout << text << '\n';
        });
    }
    if (install) {
        setUpLogging(defaultConfigDirectory());
        const auto installed = createInstaller(executable_)->install();
        if (!installed) {
            std::cerr << "VeloPlay was not installed: " << installed.error().message << '\n';
            return 1;
        }
        std::cout << "VeloPlay is installed: " << install::utf8(installed.value()) << '\n';
        return 0;
    }
    return std::nullopt;
}

void LinuxApplication::waitForPredecessor() {
    for (std::size_t i = 1; i < arguments_.size(); ++i) {
        if (arguments_[i] == "--through-sony") throughSony_ = true;
        if (arguments_[i] != "--after") continue;
        startedAfter_ = true;
        const long id = i + 1 < arguments_.size() ? std::strtol(arguments_[i + 1].c_str(), nullptr, 10) : 0;
        if (id <= 0) continue;
        // It holds the log file, the settings and the ports until it has ended. If it never does, go on anyway.
        for (int waited = 0; waited < 150 && kill(static_cast<pid_t>(id), 0) == 0; ++waited) SDL_Delay(100);
    }
}

void LinuxApplication::startSuccessor(const std::filesystem::path& program) {
    // As on Windows: the new instance starts now, while this window is in front, and waits for this one to end.
    successorProgram_ = program;
    const std::string path = program.string();
    const std::string after = std::to_string(getpid());
    char* argv[] = {const_cast<char*>(path.c_str()), const_cast<char*>("--after"), const_cast<char*>(after.c_str()), nullptr};
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    // Its own process group: it must not end with this one when the terminal it was started from closes.
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attributes, 0);
    pid_t child = 0;
    const int failed = posix_spawn(&child, path.c_str(), nullptr, &attributes, argv, environ);
    posix_spawnattr_destroy(&attributes);
    if (failed != 0) {
        spdlog::warn("Restart: the new instance could not be started now ({}); trying again at the end", failed);
        return;
    }
    successorStarted_ = true;
}

void LinuxApplication::openAddress(const std::string& address) {
    if (!openWithDesktop(address)) spdlog::warn("Could not open {} with the desktop", address.substr(0, 48));
}

bool LinuxApplication::inFront() const {
    return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

bool LinuxApplication::createWindow() {
    // The same name as the menu entry installing makes, so the dock shows one icon, not two.
    SDL_SetHint(SDL_HINT_APP_ID, kDesktopAppId);
    SDL_SetHint(SDL_HINT_APP_NAME, "VeloPlay");
    // The screen saver may come while VeloPlay sits in its menu; only a stream holds it off (see frame()).
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "1");
    // The mouse as it moves, not where the pointer is, while it belongs to the game.
    SDL_SetHint(SDL_HINT_MOUSE_RELATIVE_MODE_CENTER, "1");
    // Sound starts here on the main thread: SDL counts its parts without a lock, so the sound threads only look.
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_AUDIO)) {
        spdlog::error("SDL could not start its video part: {}", SDL_GetError());
        return false;
    }
    spdlog::info("Window system: {}", SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);
    window_ = SDL_CreateWindow("VeloPlay", kDefaultWidth, kDefaultHeight,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    if (!window_) {
        spdlog::error("SDL could not open a window: {}", SDL_GetError());
        return false;
    }
    SDL_SetWindowMinimumSize(window_, kMinWidth, kMinHeight);
    // The icon next to the program, as installing puts it there too.
    const std::filesystem::path iconFile = executable_.parent_path() / "veloplay.png";
    if (SDL_Surface* icon = SDL_LoadPNG(iconFile.c_str())) {
        SDL_SetWindowIcon(window_, icon);
        SDL_DestroySurface(icon);
    }
    gl_ = SDL_GL_CreateContext(window_);
    if (!gl_) {
        spdlog::error("OpenGL 3.3 is not available: {}", SDL_GetError());
        return false;
    }
    SDL_GL_MakeCurrent(window_, gl_);
    SDL_GL_SetSwapInterval(1);
    const auto text = [this](GLenum name) {
        const auto* value = reinterpret_cast<const char*>(getString_(name));
        return std::string(value ? value : "");
    };
    // Nothing of OpenGL is linked; these few come through SDL like everything the renderer uses.
    bindFramebuffer_ = reinterpret_cast<void (*)(unsigned, unsigned)>(glFunction("glBindFramebuffer"));
    getString_ = reinterpret_cast<const unsigned char* (*)(unsigned)>(glFunction("glGetString"));
    viewport_ = reinterpret_cast<void (*)(int, int, int, int)>(glFunction("glViewport"));
    clearColor_ = reinterpret_cast<void (*)(float, float, float, float)>(glFunction("glClearColor"));
    clear_ = reinterpret_cast<void (*)(unsigned)>(glFunction("glClear"));
    if (!bindFramebuffer_ || !getString_ || !viewport_ || !clearColor_ || !clear_) {
        spdlog::error("OpenGL 3.3 is not available: basic functions are missing");
        return false;
    }
    glVendor_ = text(GL_VENDOR);
    glRenderer_ = text(GL_RENDERER);
    spdlog::info("Graphics: {} ({}), OpenGL {}", glRenderer_, glVendor_, text(GL_VERSION));
    return true;
}

void LinuxApplication::configureController(const std::filesystem::path& dataDirectory) {
    protector_ = createSecretServiceProtector();
    AppController::Options options;
    options.configPath = dataDirectory / "config.json";
    options.protector = protector_.get();
    // One queue for window, keys and controllers: this loop fetches it and hands the controllers' events on.
    options.controllerBackend = input::createSdlControllerBackend(false);
    controllerBackend_ = options.controllerBackend.get(); // the controller keeps it as long as it lives
    options.openHelp = [this](std::string_view topic) {
        const bool german = ui::resolveLanguage(controller_->config().language, systemLanguage_) == ui::Language::German;
        openAddress(helpUrl(topic, german));
    };
    options.openAddress = [this](const std::string& url) { openAddress(url); };
    options.windowAction = [this](WindowAction action) { pendingWindowAction_ = action; };
    options.sessionFactory = session::chiakiSessionFactory();
    options.inputInterval = std::chrono::microseconds{2000};
    // No embedded browser here: Sony's page opens in the user's own one, and the address of the empty page
    // after signing in is pasted into the field, the way the Windows build does without WebView2.
    options.showLogin = [this](const std::string& url) {
        // Shown means waiting: the address arrives through the field the settings show meanwhile.
        return openWithDesktop(url);
    };

    // Made before the sound output, which hands it every block of sound from its first one on.
    recorder_ = std::make_unique<VideoRecorder>();
    audioOutput_ = std::make_unique<SdlAudioOutput>();
    audioOutput_->setTap([recorder = recorder_.get()](std::span<const std::int16_t> sound, int rate, int channels) {
        recorder->addSound(sound, rate, channels);
    });
    microphone_ = std::make_unique<SdlAudioInput>();
    options.microphone = microphone_.get();
    // On a cable the DualSense is a sound device of its own, which plays its haptics.
    hapticsOutput_ = std::make_unique<SdlAudioOutput>(std::vector<std::string>{"DualSense", "Wireless Controller"});
    if (videoReady_) {
        pipeline_ = std::make_unique<media::MediaPipeline>(
            media::MediaPipeline::Options{nullptr, audioOutput_.get(), hapticsOutput_.get()});
        options.mediaSink = pipeline_.get();
    }
    http_ = createCurlHttpClient("VeloPlay/" VELOPLAY_VERSION);
    options.http = http_.get();
    options.installDir = executable_.parent_path();
    options.restart = [this] {
        restartRequested_ = true;
        // After an update the running file is VeloPlay.old; the new one has the program's own name.
        startSuccessor(executable_.parent_path() / update::kThisPackage.programFile);
        quit_ = true;
    };
    options.takeScreenshot = [this] {
        if (!saving_.valid()) grabber_.request();
    };
    picturesFolder_ = userDirectory(UserDirectory::Pictures) / "VeloPlay";
    options.picturesFolder = picturesFolder_;
    videosFolder_ = userDirectory(UserDirectory::Videos) / "VeloPlay";
    frameLogFolder_ = dataDirectory / "recordings";
    recordAvailable_ = VideoRecorder::available(&recordMissing_);
    if (!recordAvailable_) spdlog::info("Recording is not available: {}", recordMissing_);
    options.videosFolder = videosFolder_;
    captureScan_ = std::jthread([this](std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::vector<CaptureDevice> found = listCaptureDevices();
            {
                std::lock_guard lock(captureMutex_);
                if (found != captureDevices_) {
                    for (const CaptureDevice& device : found) {
                        if (std::find(captureDevices_.begin(), captureDevices_.end(), device) == captureDevices_.end()) {
                            spdlog::info("Video source found: {}", device.name);
                        }
                    }
                    captureDevices_ = std::move(found);
                }
            }
            for (int slice = 0; slice < 50 && !stop.stop_requested(); ++slice) SDL_Delay(100);
        }
    });
    options.captureDevices = [this] {
        std::lock_guard lock(captureMutex_);
        return captureDevices_;
    };
    options.captureFactory = [] { return std::unique_ptr<session::ISession>(std::make_unique<CaptureSession>()); };
    options.isAway = [](std::uint32_t address) { return !net::isAtHome(address, net::localSubnets()); };
    if (session::psnAvailable()) options.psnConsoles = [](const std::string& token) { return session::listPsnConsoles(token); };
    options.alwaysThroughSony = throughSony_;
    if (throughSony_) spdlog::info("Started with --through-sony: consoles are reached through Sony's servers");
    // Discord's card while a game is streamed; the socket finds Discord's desktop app if it runs.
    options.discordPipe = [] { return discord::createPipe(); };
    options.processId = static_cast<std::uint32_t>(getpid());
    options.systemLanguage = systemLanguage_;
    options.toggleRecording = [this] { recordToggle_ = true; };
    options.keepControllerTouchpadFromDesktop = [this] {
        if (touchpadRule_.valid()) return;
        touchpadRuleFailed_ = false;
        spdlog::info("Controller touchpad: writing the rule that keeps it away from the desktop");
        touchpadRule_ = std::async(std::launch::async, [] { return writeTouchpadRule(); });
    };
    options.controllerTouchpad = [this] {
        UiModel::ControllerTouchpadView view;
        std::error_code ec;
        view.offered = std::filesystem::exists("/etc/udev/rules.d", ec);
        if (touchpadRule_.valid() && touchpadRule_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            touchpadRuleFailed_ = !touchpadRule_.get();
            spdlog::info("Controller touchpad: {}", touchpadRuleFailed_ ? "the rule was not written" : "rule written");
        }
        view.working = touchpadRule_.valid();
        view.kept = std::filesystem::exists(kTouchpadRuleFile, ec);
        if (touchpadRuleFailed_ && !view.kept) view.error = "pkexec ended without writing the rule";
        return view;
    };
    installer_ = createInstaller(executable_);
    options.installer = installer_.get();
    options.startInstalled = [this](const std::filesystem::path& program) {
        restartRequested_ = true;
        startSuccessor(program);
        quit_ = true;
    };
    refreshInstallation(executable_);
    options.recording = [this] {
        UiModel::RecordingView view;
        view.available = recordAvailable_;
        view.missing = recordMissing_;
        view.starting = recordStarting_.valid();
        view.running = !view.starting && !recordStopping_.valid() && recorder_->running();
        if (view.running) view.seconds = recorder_->seconds();
        return view;
    };
    options.openFolder = [this](const std::filesystem::path& folder) {
        std::error_code ignored;
        std::filesystem::create_directories(folder, ignored);
        openAddress(folder.string());
    };
    controller_ = std::make_unique<AppController>(std::move(options));
    // What this laptop or PC can do decides the picture settings of the first start.
    HardwareInfo hardware = describeLinux(glVendor_, glRenderer_);
    if (const SDL_DisplayMode* mode = SDL_GetDesktopDisplayMode(SDL_GetDisplayForWindow(window_))) {
        hardware.screenWidth = mode->w;
        hardware.screenHeight = mode->h;
        hardware.screenHz = mode->refresh_rate;
    }
    controller_->setHardware(hardware);
    controller_->start();
}

int LinuxApplication::run() {
    if (const std::optional<int> done = runCommand()) return *done;
    waitForPredecessor();
    // libchiaki's own requests to Sony's servers look for the certificates here; see caBundle().
    if (const std::string bundle = caBundle(); !bundle.empty()) setenv("VP_CA_BUNDLE", bundle.c_str(), 0);
    // A write to a connection that closed must not end the program.
    signal(SIGPIPE, SIG_IGN);
    const std::filesystem::path dataDirectory = defaultConfigDirectory();
    setUpLogging(dataDirectory);
    systemLanguage_ = desktopLanguage();

    if (!createWindow()) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "VeloPlay",
                                 "VeloPlay needs a graphics driver with OpenGL 3.3.\n\nInstalling the driver for your graphics card usually fixes this.",
                                 nullptr);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplSDL3_InitForOpenGL(window_, gl_);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    imguiReady_ = true;

    std::string renderError;
    videoReady_ = videoRenderer_.create(&glFunction, renderError);
    if (!videoReady_) spdlog::error("Video output is not available: {}", renderError);
    grabber_.setFunctions(&videoRenderer_.gl());
    recordStream_.setFunctions(&videoRenderer_.gl());

    configureController(dataDirectory);

    ui_ = std::make_unique<ui::UiManager>(*controller_);
    std::string regular = firstFont({"/usr/share/fonts/truetype/open-sans/OpenSans-Regular.ttf",
                                     "/usr/share/fonts/open-sans/OpenSans-Regular.ttf",
                                     "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                                     "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                                     "/usr/share/fonts/google-noto/NotoSans-Regular.ttf"});
    std::string semibold = firstFont({"/usr/share/fonts/truetype/open-sans/OpenSans-Semibold.ttf",
                                      "/usr/share/fonts/open-sans/OpenSans-Semibold.ttf",
                                      "/usr/share/fonts/truetype/noto/NotoSans-SemiBold.ttf",
                                      "/usr/share/fonts/noto/NotoSans-SemiBold.ttf",
                                      "/usr/share/fonts/google-noto/NotoSans-SemiBold.ttf"});
    if (regular.empty()) regular = fontconfigFont("sans-serif:weight=regular");
    if (semibold.empty()) semibold = fontconfigFont("sans-serif:weight=semibold");
    spdlog::info("Fonts: {}, {}", regular.empty() ? "built-in" : regular, semibold.empty() ? "as regular" : semibold);
    fonts_ = ui::loadFonts(regular.c_str(), semibold.c_str());
    applyScale();

    stallWatch_.start(std::chrono::milliseconds{1500}, [](const diag::StallReport& report) {
        if (report.finished) spdlog::warn("The window was stuck for {} ms in: {}", report.duration.count(), report.step);
        else spdlog::warn("The window has been stuck for {} ms in: {}", report.duration.count(), report.step);
    });

    SDL_ShowWindow(window_);
    if (controller_->config().window.fullscreen) toggleFullscreen(false);
    if (startedAfter_) {
        SDL_RaiseWindow(window_);
        spdlog::info("Window: started after an update");
    }

    while (!quit_) {
        diag::loopTurned();
        SDL_Event event;
        while (SDL_PollEvent(&event)) handleEvent(event);
        if (quit_) break;
        frame();
    }

    if (screenSaverHeld_) SDL_EnableScreenSaver();
    if (recordStarting_.valid()) (void)recordStarting_.get();
    if (recordStopping_.valid()) recordingStopped(recordStopping_.get());
    if (recorder_ && recorder_->running()) recordingStopped(recorder_->stop());
    stallWatch_.stop();
    if (restartRequested_) {
        captureScan_.request_stop();
        ui_.reset();
        controller_.reset();
        if (!successorStarted_) {
            const std::filesystem::path program =
                successorProgram_.empty() ? executable_.parent_path() / update::kThisPackage.programFile : successorProgram_;
            startSuccessor(program);
        }
    }
    return 0;
}

void LinuxApplication::handleEvent(const SDL_Event& event) {
    if (controller_) {
        switch (event.type) {
            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
            case SDL_EVENT_JOYSTICK_ADDED:
            case SDL_EVENT_JOYSTICK_REMOVED:
                if (controllerBackend_) input::handleSdlControllerEvent(*controllerBackend_, &event);
                return;
            default: break;
        }
    }
    // Keys and mouse that belong to the game never reach the interface.
    if (controller_ && routeInput(event)) return;
    if (imguiReady_) ImGui_ImplSDL3_ProcessEvent(&event);
    switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: quit_ = true; break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            spdlog::info("Window is no longer in front");
            // Whatever was held is let go: the key that comes up later goes to another window.
            if (gameInput_) setGameInput(false);
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED: spdlog::info("Window is in front"); break;
        case SDL_EVENT_WINDOW_MINIMIZED:
            minimised_ = true;
            spdlog::info("Window minimised");
            break;
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
            if (minimised_) spdlog::info("Window restored");
            minimised_ = false;
            break;
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED: applyScale(); break;
        case SDL_EVENT_KEY_DOWN:
            if (event.key.scancode == SDL_SCANCODE_F11 && !event.key.repeat) toggleFullscreen();
            break;
        default: break;
    }
}

bool LinuxApplication::routeInput(const SDL_Event& event) {
    const bool capturing = controller_->capturingKey();
    if (!gameInput_ && !capturing) return false;
    int code = 0;
    bool down = false;
    switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            if (event.key.repeat) return gameInput_ || capturing;
            code = keyCodeFor(event.key.scancode);
            down = true;
            break;
        case SDL_EVENT_KEY_UP: code = keyCodeFor(event.key.scancode); break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
            code = mouseButtonCode(event.button.button);
            down = true;
            break;
        case SDL_EVENT_MOUSE_BUTTON_UP: code = mouseButtonCode(event.button.button); break;
        case SDL_EVENT_MOUSE_WHEEL: {
            const int notches = static_cast<int>(event.wheel.integer_y) * (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1);
            if (capturing) {
                if (notches != 0) controller_->keyCaptured(notches > 0 ? input::key::WheelUp : input::key::WheelDown);
            } else if (notches != 0) {
                controller_->keyboardMouse().wheel(notches, std::chrono::steady_clock::now());
            }
            return true;
        }
        case SDL_EVENT_MOUSE_MOTION: {
            if (!gameInput_) return false;
            // Relative movement comes in fractions on some systems; the rest waits for the next event.
            mouseRestX_ += event.motion.xrel;
            mouseRestY_ += event.motion.yrel;
            const int dx = static_cast<int>(mouseRestX_);
            const int dy = static_cast<int>(mouseRestY_);
            mouseRestX_ -= static_cast<float>(dx);
            mouseRestY_ -= static_cast<float>(dy);
            if (dx != 0 || dy != 0) controller_->keyboardMouse().mouseMoved(dx, dy);
            return true;
        }
        default: return false;
    }
    if (capturing) {
        // The settings wait for a key. Esc ends the waiting without one; key-ups are of no interest.
        if (down) controller_->keyCaptured(code == input::key::Escape ? 0 : code);
        return true;
    }
    if (code == 0) return true;
    // Esc, F-keys of the program and F11 stay with it: the menu, the display, full screen.
    if (input::isReservedKey(code)) return false;
    controller_->keyboardMouse().key(code, down);
    return true;
}

void LinuxApplication::setGameInput(bool on) {
    if (on == gameInput_ || !window_) return;
    gameInput_ = on;
    // Relative mode hides the pointer, keeps it in the window and reports the movement of the hand.
    if (!SDL_SetWindowRelativeMouseMode(window_, on) && on) {
        spdlog::warn("The mouse cannot be held in the window ({}); looking around with it may stop at the edge", SDL_GetError());
    }
    ImGuiIO& io = ImGui::GetIO();
    if (on) {
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    } else {
        io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange);
        mouseRestX_ = mouseRestY_ = 0.0f;
        if (controller_) controller_->keyboardMouse().releaseAll();
    }
    spdlog::info("Keys and mouse belong to the {}", on ? "game" : "program");
}

void LinuxApplication::toggleFullscreen(bool remember) {
    // The desktop's own full screen: no change of the screen's mode, and switching away stays quick.
    fullscreen_ = !fullscreen_;
    SDL_SetWindowFullscreenMode(window_, nullptr);
    if (!SDL_SetWindowFullscreen(window_, fullscreen_)) {
        spdlog::warn("Full screen did not change: {}", SDL_GetError());
        fullscreen_ = !fullscreen_;
        return;
    }
    spdlog::info("Window is {}", fullscreen_ ? "full screen" : "a window");
    if (remember && controller_ && controller_->config().window.fullscreen != fullscreen_) {
        controller_->config().window.fullscreen = fullscreen_;
        controller_->configChanged();
    }
}

void LinuxApplication::applyWindowAction(WindowAction action) {
    switch (action) {
        case WindowAction::Minimize: SDL_MinimizeWindow(window_); break;
        case WindowAction::ToggleMaximize:
            if (!fullscreen_) {
                if (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(window_);
                else SDL_MaximizeWindow(window_);
            }
            break;
        case WindowAction::ToggleFullscreen: toggleFullscreen(); break;
        case WindowAction::Close: quit_ = true; break;
    }
}

void LinuxApplication::applyScale() {
    // The desktop's scaling apart from the pixel density, which ImGui handles by itself: points stay points.
    const float display = SDL_GetWindowDisplayScale(window_);
    const float density = SDL_GetWindowPixelDensity(window_);
    scale_ = std::clamp(density > 0.0f ? display / density : display, 0.5f, 4.0f);
    if (ui_) ui_->configure(fonts_, scale_, systemLanguage_);
}

void LinuxApplication::frame() {
    if (minimised_ || (SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED)) {
        controller_->update();
        diag::setStep("collecting what to show");
        controller_->fillModel(model_);
        logStreamSummary(std::chrono::steady_clock::now());
        diag::setStep("idle, minimised");
        SDL_Delay(50);
        return;
    }
    if (const std::optional<WindowAction> action = std::exchange(pendingWindowAction_, std::nullopt)) {
        applyWindowAction(*action);
        if (quit_) return;
    }

    if (pipeline_) controller_->noteDroppedFrames(pipeline_->stats().droppedFrames);
    controller_->notePictureTiming(pacer_.unevenness(), std::chrono::duration<double, std::milli>(pacer_.reserve()).count(),
                                   insideMs_, std::chrono::duration<double, std::milli>(pacer_.addedDelay()).count());
    controller_->update();
    diag::setStep("collecting what to show");
    controller_->fillModel(model_);
    model_.gpuName = glRenderer_;
    const std::string vendor = glVendor_ + " " + glRenderer_;
    model_.gpuVendor = vendor.find("NVIDIA") != std::string::npos ? UiModel::GpuVendor::Nvidia
                       : (vendor.find("AMD") != std::string::npos || vendor.find("Radeon") != std::string::npos) ? UiModel::GpuVendor::Amd
                       : vendor.find("Intel") != std::string::npos ? UiModel::GpuVendor::Intel
                                                                   : UiModel::GpuVendor::Other;
    model_.reducedMotion = false;
    model_.rtxVideoAvailable = false;
    model_.amdFrcActive = false;
    // In a window the desktop draws the frame. In full screen the desktop's own bars cannot come over the
    // picture (the user found the Super key a poor way out); VeloPlay's bar slides in at the top edge instead,
    // with minimise, window and close, as on Windows.
    model_.ownWindowBar = fullscreen_;
    model_.platform.os = HardwareInfo::Os::Linux;
    model_.platform.frameGeneration = videoRenderer_.frameGenerationAvailable();
    model_.platform.upscaling = videoReady_;
    model_.platform.deband = videoReady_;
    model_.platform.looks = videoReady_;
    model_.platform.amdFrc = false;
    model_.platform.captureCards = true;
    model_.platform.embeddedSignIn = false;
    model_.windowFullscreen = fullscreen_;
    model_.windowMaximized = (SDL_GetWindowFlags(window_) & SDL_WINDOW_MAXIMIZED) != 0;
    model_.clock = localClock();
    if (const auto now = std::chrono::steady_clock::now(); now >= nextDisplayCheck_) {
        nextDisplayCheck_ = now + std::chrono::seconds(1);
        const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window_));
        displayHz_ = mode && mode->refresh_rate > 1.0f ? static_cast<double>(mode->refresh_rate) : 0.0;
    }
    model_.displayHz = displayHz_;

    const bool streaming = model_.state == AppState::Streaming;
    // A stream is watched, not typed at: the screen saver and the screen's sleep wait while one runs.
    if (streaming != screenSaverHeld_) {
        if (streaming) SDL_DisableScreenSaver();
        else SDL_EnableScreenSaver();
        screenSaverHeld_ = streaming;
    }
    const AppConfig& config = controller_->config();
    if (pipeline_) {
        const auto now = std::chrono::steady_clock::now();
        pipeline_->setHapticsEnabled(config.controller.rumble);
        if (!streaming) {
            forgetPictures();
        } else if (model_.stream && now >= nextRateSample_) {
            nextRateSample_ = now + std::chrono::seconds(1);
            inputRate_.addSample(model_.stream->streamFps);
        }
        const bool halved = streaming && inputRate_.fps() == 60 && cadence_.halved();
        const int gameFps = halved ? 30 : inputRate_.fps();
        const int wanted = media::frameGenerationFactorFor(config.graphics.frameGenerationFactor, gameFps, displayHz_);
        const bool available = videoRenderer_.frameGenerationAvailable();
        skipRepeats_ = halved && available && wanted > 1;
        pacer_.configure(available ? wanted : 1, std::chrono::microseconds(1'000'000 / (skipRepeats_ ? 30 : inputRate_.fps())),
                         std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::duration<double, std::milli>(refreshMs_)));
        std::chrono::milliseconds reserve{config.network.jitter == JitterMode::Stable ? 140 : 80};
        if (config.network.jitter == JitterMode::Auto) {
            const std::uint64_t gaps = streaming ? pipeline_->stats().audioUnderruns : 0;
            if (streaming && soundReserve_.update(gaps, now)) {
                spdlog::info("Sound: {} ms kept against late packets now (gaps in the sound so far: {})",
                             soundReserve_.reserve().count(), gaps);
            }
            reserve = soundReserve_.reserve();
        }
        pipeline_->setAudioDelay(reserve + std::chrono::duration_cast<std::chrono::milliseconds>(pacer_.addedDelay()));

        if (model_.stream && videoRenderer_.hasPicture()) {
            const media::MediaPipeline::Stats media = pipeline_->stats();
            StreamStats& stats = *model_.stream;
            stats.presenting = true;
            stats.outputFps = static_cast<float>(shownMeter_.rates(now).framesPerSecond);
            stats.gameFps = halved ? 30 : 0;
            stats.inputWidth = videoRenderer_.width();
            stats.inputHeight = videoRenderer_.height();
            stats.outputWidth = videoRenderer_.shownWidth();
            stats.outputHeight = videoRenderer_.shownHeight();
            stats.scaler = videoRenderer_.scalerName();
            stats.insidePcMs = static_cast<float>(insideMs_);
            stats.frameGenerationFactor = wanted;
            stats.frameGenerationAvailable = available;
            stats.frameGenerationActive = available && pacer_.factor() > 1;
            stats.frameGenerationEngine = videoRenderer_.frameGeneratorName();
            stats.addedLatencyMs = std::chrono::duration<float, std::milli>(pacer_.addedDelay()).count();
            stats.unevenMs = pacer_.unevenness();
            stats.decodeMs = static_cast<float>(media.decodeMs);
            stats.decodeMaxMs = static_cast<float>(media.decodeMaxMs);
            stats.decoder = media.decoder;
            stats.droppedFrames = media.droppedFrames;
            stats.frameTimes.assign(frameTimes_.begin(), frameTimes_.end());
        }
    }
    logStreamSummary(std::chrono::steady_clock::now());

    diag::setStep("laying out the screen");
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ui_->feedNavigation(model_);
    ImGui::NewFrame();
    ui_->draw(model_);
    ImGui::Render();
    // Not with a capture card: nothing goes from here to its console, and the pointer would be held for nothing.
    setGameInput(streaming && !model_.session.capture && config.controller.keyboardMouse.enabled &&
                 ui_->pictureOwnsInput() && !controller_->capturingKey() && inFront());

    // Swapping below waits for the screen; the picture is chosen as late as this loop allows.
    GlVideoRenderer::Output output;
    bool freshPicture = false;
    if (pipeline_ && streaming) {
        diag::setStep("taking over a picture");
        const auto now = std::chrono::steady_clock::now();
        videoRenderer_.wantGeneration(pacer_.factor() > 1);
        if (lastRefresh_ != std::chrono::steady_clock::time_point{}) {
            const double gap = std::chrono::duration<double, std::milli>(now - lastRefresh_).count();
            if (gap > 1.0 && gap < 50.0) refreshMs_ = refreshMs_ == 0.0 ? gap : refreshMs_ * 0.9 + gap * 0.1;
        }
        lastRefresh_ = now;
        const auto slack = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(refreshMs_ * 0.5));
        for (media::VideoFrame& picture : pipeline_->takeFrames()) waiting_.push_back(std::move(picture));
        while (waiting_.size() > 8) {
            waiting_.pop_front();
            ++overflowPictures_;
        }
        while (!waiting_.empty()) {
            const bool repeat = skipRepeats_ && cadence_.isRepeat(videoRenderer_.submitted() + 1);
            if (!repeat && !pacer_.wantsNext(now, slack) && waiting_.size() <= 4) break;
            const media::VideoFrame picture = std::move(waiting_.front());
            waiting_.pop_front();
            const bool taken = videoRenderer_.submit(picture, !repeat);
            while (const auto change = videoRenderer_.takeChange()) cadence_.add(change->picture, change->mean);
            if (cadence_.halved() != loggedHalved_) {
                loggedHalved_ = cadence_.halved();
                const double* lately = cadence_.recent();
                spdlog::info("Game rate: {} (change of the last three pictures: {:.2f}, {:.2f}, {:.2f} levels)",
                             loggedHalved_ ? "30 fps, every other picture repeats" : "as the stream", lately[0], lately[1], lately[2]);
            }
            if (!taken || repeat) continue;
            const bool stamped = picture.decodedAt != std::chrono::steady_clock::time_point{};
            pacer_.onFrame(videoRenderer_.newestId(), stamped ? picture.decodedAt : now);
            arrivals_[arrivalsNext_++ % arrivals_.size()] = {videoRenderer_.newestId(), picture.receivedAt};
            if (videoRenderer_.frameGenerationAvailable() && loggedGenerator_ != videoRenderer_.frameGeneratorName()) {
                loggedGenerator_ = videoRenderer_.frameGeneratorName();
                spdlog::info("Frame generation: {}", loggedGenerator_);
            }
            if (!videoRenderer_.frameGenerationAvailable() && !generatorFailureLogged_) {
                generatorFailureLogged_ = true;
                spdlog::warn("Frame generation is not available: {}", videoRenderer_.frameGeneratorError());
            }
        }
        output.choice = pacer_.choose(now, slack);
        output.upscale = config.graphics.superResolutionLevel > 0;
        output.sharpness = config.graphics.sharpness;
        output.deband = config.graphics.deband;
        output.look = config.graphics.look;
        output.lookStrength = config.graphics.lookStrength;
        if (output.choice.valid && output.choice == lastChoice_ && videoRenderer_.hasPicture()) ++repeatedRefreshes_;
        if (output.choice.valid && output.choice.step == 0 && output.choice.base > lastRealShown_) {
            if (lastRealShown_ > 0 && output.choice.base > lastRealShown_ + 1) skippedPictures_ += output.choice.base - lastRealShown_ - 1;
            lastRealShown_ = output.choice.base;
        }
        if (output.choice.valid && !(output.choice == lastChoice_)) {
            freshPicture = true;
            lastChoice_ = output.choice;
            shownMeter_.addVideo(0, 0);
            if (output.choice.step == 0) {
                for (const auto& [id, received] : arrivals_) {
                    if (id != output.choice.base || received == std::chrono::steady_clock::time_point{}) continue;
                    const double ms = std::chrono::duration<double, std::milli>(now - received).count();
                    insideMs_ = insideMs_ == 0.0 ? ms : insideMs_ * 0.9 + ms * 0.1;
                }
            }
            if (lastPicture_ != std::chrono::steady_clock::time_point{}) {
                frameTimes_.push_back(std::chrono::duration<float, std::milli>(now - lastPicture_).count());
                if (frameTimes_.size() > 90) frameTimes_.pop_front();
            }
            lastPicture_ = now;
        }
    }
    const bool showVideo = streaming && videoRenderer_.hasPicture();

    int width = 0;
    int height = 0;
    SDL_GetWindowSizeInPixels(window_, &width, &height);
    diag::setStep("drawing");
    bindFramebuffer_(0x8D40 /* GL_FRAMEBUFFER */, 0);
    viewport_(0, 0, width, height);
    const ImVec4& background = ui::palette().background;
    clearColor_(showVideo ? 0.0f : background.x, showVideo ? 0.0f : background.y, showVideo ? 0.0f : background.z, 1.0f);
    clear_(GL_COLOR_BUFFER_BIT);
    if (showVideo) videoRenderer_.draw(width, height, output);
    // The region of the picture without the black bars, counted from the top left.
    const int shownWidth = std::min(std::max(0, videoRenderer_.shownWidth()), width);
    const int shownHeight = std::min(std::max(0, videoRenderer_.shownHeight()), height);
    const int left = videoRenderer_.shownLeft();
    const int top = videoRenderer_.shownTop();
    // Before the overlays: a screenshot holds the game, not VeloPlay's display or notes.
    if (showVideo && grabber_.busy()) grabber_.capture(left, top, shownWidth, shownHeight, height);
    handleRecording(showVideo,
                    !freshPicture ? media::ShownKind::Repeated
                                  : (output.choice.step == 0 ? media::ShownKind::Real : media::ShownKind::Computed),
                    std::chrono::steady_clock::now());
    viewport_(0, 0, width, height);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    diag::setStep("presenting");
    SDL_GL_SetSwapInterval(streaming && !config.graphics.vsync ? 0 : 1);
    SDL_GL_SwapWindow(window_);
    collectScreenshot();
    diag::setStep("idle");

    // The menu does not need 60 redraws per second while another window is in front. A running stream does.
    if (!streaming && !inFront()) SDL_Delay(40);
}

void LinuxApplication::forgetPictures() {
    videoRenderer_.clear();
    waiting_.clear();
    lastRealShown_ = 0;
    frameTimes_.clear();
    lastPicture_ = {};
    lastChoice_ = {};
    pacer_.reset();
    soundReserve_.restart(std::chrono::steady_clock::now());
    lastRefresh_ = {};
    refreshMs_ = 0.0;
    insideMs_ = 0.0;
    arrivals_ = {};
    inputRate_.reset(model_.requestedStream.fps);
    cadence_.reset();
    skipRepeats_ = false;
    loggedHalved_ = false;
    loggedGenerator_.clear();
    nextRateSample_ = {};
}

void LinuxApplication::handleRecording(bool showVideo, media::ShownKind kind, std::chrono::steady_clock::time_point now) {
    using namespace std::chrono_literals;
    if (recordStopping_.valid() && recordStopping_.wait_for(0s) == std::future_status::ready) recordingStopped(recordStopping_.get());
    if (recordStarting_.valid() && recordStarting_.wait_for(0s) == std::future_status::ready) {
        const Result<bool> started = recordStarting_.get();
        if (!started) {
            spdlog::warn("Recording did not start: {}", started.error().message);
            controller_->captureFinished(false, {}, started.error().message, true);
        }
    }
    const bool busy = recordStarting_.valid() || recordStopping_.valid();
    const bool running = !busy && recorder_->running();

    int windowWidth = 0;
    int windowHeight = 0;
    SDL_GetWindowSizeInPixels(window_, &windowWidth, &windowHeight);
    const int shownWidth = std::min(std::max(0, videoRenderer_.shownWidth()), windowWidth);
    const int shownHeight = std::min(std::max(0, videoRenderer_.shownHeight()), windowHeight);
    const int left = videoRenderer_.shownLeft();
    const int top = videoRenderer_.shownTop();
    const int width = shownWidth & ~1;
    const int height = shownHeight & ~1;

    if (std::exchange(recordToggle_, false) && !busy) {
        if (running) {
            stopRecording("asked to");
            return;
        }
        if (!recordAvailable_ || !showVideo) return;
        const Result<std::filesystem::path> file = media::freeCapturePath(videosFolder_, localNow(), "mp4");
        if (!file) {
            controller_->captureFinished(false, {}, file.error().message, true);
            return;
        }
        recordFile_ = file.value();
        recordStream_.reset();
        const int fps = std::min(60, static_cast<int>(std::lround(displayHz_ > 1.0 ? displayHz_ : 60.0)));
        recordFps_ = fps;
        lastRecorded_ = {};
        recordStarting_ = std::async(std::launch::async, [recorder = recorder_.get(), path = recordFile_, width, height, fps] {
            return recorder->start(path, width, height, fps);
        });
        return;
    }
    if (!running) return;
    if (!showVideo) {
        stopRecording("the stream ended");
        return;
    }
    if (width != recorder_->width() || height != recorder_->height()) {
        stopRecording("the window changed its size");
        return;
    }
    const auto interval = std::chrono::duration<double>(0.75 / recordFps_);
    if (lastRecorded_ == std::chrono::steady_clock::time_point{} || now - lastRecorded_ >= interval) {
        if (recordStream_.capture(left, top, width, height, windowHeight, kind, now)) lastRecorded_ = now;
    }
    while (auto picture = recordStream_.take(recorder_->buffer())) {
        recorder_->addPicture(std::move(picture->bgra), picture->kind, picture->at);
    }
}

void LinuxApplication::stopRecording(const char* why) {
    spdlog::info("Recording ends: {}", why);
    recordStream_.reset();
    recordStopping_ = std::async(std::launch::async, [recorder = recorder_.get()] { return recorder->stop(); });
}

void LinuxApplication::recordingStopped(Result<record::RecordingSummary> outcome) {
    if (!outcome) {
        spdlog::warn("Recording failed: {}", outcome.error().message);
        if (controller_) controller_->captureFinished(false, {}, outcome.error().message, true);
        return;
    }
    const record::RecordingSummary& summary = outcome.value();
    std::error_code ignored;
    std::filesystem::create_directories(frameLogFolder_, ignored);
    std::filesystem::path log = frameLogFolder_ / summary.file.filename();
    log.replace_extension(".frames.txt");
    std::ofstream(log, std::ios::binary) << summary.log.text();
    if (controller_) controller_->captureFinished(true, install::utf8(videosFolder_), {}, true);
}

void LinuxApplication::collectScreenshot() {
    if (saving_.valid() && saving_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const Saved saved = saving_.get();
        controller_->captureFinished(saved.ok, saved.where, saved.detail);
    }
    if (const char* why = grabber_.failure(); why && *why && !grabber_.busy() && !saving_.valid()) {
        controller_->captureFinished(false, {}, why);
        grabber_.release();
        return;
    }
    std::optional<media::Image> image = grabber_.take();
    if (!image) return;
    saving_ = std::async(std::launch::async, [picture = std::move(*image), folder = picturesFolder_]() -> Saved {
        const Result<std::vector<std::uint8_t>> png = media::encodePng(picture);
        if (!png) return {false, {}, png.error().message};
        const Result<std::filesystem::path> file = media::saveCapture(folder, localNow(), "png", png.value());
        if (!file) return {false, {}, file.error().message};
        return {true, install::utf8(folder), {}};
    });
}

void LinuxApplication::logStreamSummary(std::chrono::steady_clock::time_point now) {
    if (model_.state != AppState::Streaming || !model_.stream) {
        nextSummary_ = {};
        return;
    }
    if (nextSummary_ == std::chrono::steady_clock::time_point{}) nextSummary_ = now + std::chrono::seconds(10);
    if (now < nextSummary_) return;
    nextSummary_ = now + std::chrono::seconds(10);
    const StreamStats& stream = *model_.stream;
    const media::MediaPipeline::Stats media = pipeline_ ? pipeline_->stats() : media::MediaPipeline::Stats{};
    spdlog::info("Stream: console {:.1f} fps, shown {:.1f} fps, {:.1f} Mbit/s, loss {:.1f} %, decoder {:.1f} ms "
                 "(slowest {:.1f} ms), {} decoded, {} dropped, {} failed, frame generation {}x, {:.0f} ms inside the PC, "
                 "{:.1f} ms waited for late pictures, {} refreshes without a new picture, {} pictures never shown, "
                 "{} dropped from the queue, screen {:.0f} Hz{}{}",
                 stream.streamFps, stream.outputFps, stream.bitrateMbps, stream.packetLossPercent, media.decodeMs,
                 media.decodeMaxMs, media.decodedFrames, media.droppedFrames, media.failedFrames, pacer_.factor(), insideMs_,
                 std::chrono::duration<double, std::milli>(pacer_.reserve()).count(), std::exchange(repeatedRefreshes_, 0),
                 std::exchange(skippedPictures_, 0), std::exchange(overflowPictures_, 0), displayHz_,
                 inFront() ? "" : ", window not in front", cadence_.halved() ? ", game at 30 fps (every other picture repeats)" : "");
}

} // namespace veloplay::linuxos
