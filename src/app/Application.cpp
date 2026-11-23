#include "app/Application.hpp"

#include <ctime>

#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <timeapi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <optional>
#include <utility>

#include "app/HangProbe.hpp"
#include "app/HelpLinks.hpp"
#include "app/Installer.hpp"
#include "net/Socket.hpp"
#include "util/LogFiles.hpp"
#include "input/SdlControllerBackend.hpp"
#include "update/Release.hpp"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

namespace veloplay {

namespace {

constexpr wchar_t kWindowClass[] = L"VeloPlayWindow";
constexpr int kDefaultWidth = 1100;
constexpr int kDefaultHeight = 720;
constexpr int kMinWidth = 820;
constexpr int kMinHeight = 560;
constexpr int kLogsKept = 3; // logs of earlier runs, next to the current one

// While the window is moved or resized, or its menu is open, Windows runs a
// loop of its own inside our message handler. Our loop stands still for that
// time, and nothing is wrong.
constexpr char kModalStep[] = "moving or resizing the window";

std::wstring toWide(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

void setUpLogging(const std::filesystem::path& directory) {
    std::vector<spdlog::sink_ptr> sinks;
    // The debugger channel is slow and takes a system-wide lock, so it is
    // only used when someone listens.
    if (IsDebuggerPresent()) sinks.push_back(std::make_shared<spdlog::sinks::msvc_sink_mt>());
    try {
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        // One file per run keeps the log small enough to attach to a bug
        // report. The logs of the runs before stay as veloplay.1.log and so
        // on: after a hang the program is started again before anyone looks.
        const std::filesystem::path file = rotateLogFiles(directory, "veloplay", kLogsKept);
        sinks.push_back(std::make_shared<spdlog::sinks::basic_file_sink_mt>(file.string(), true));
    } catch (const spdlog::spdlog_ex&) {
        // No log file, for example on a read-only profile. The client still works.
    }
    auto logger = std::make_shared<spdlog::logger>("veloplay", sinks.begin(), sinks.end());
    // Warnings reach the disk at once, the rest once a second: flushing every
    // line would slow down the threads that log from inside the stream.
    logger->flush_on(spdlog::level::warn);
    spdlog::set_default_logger(std::move(logger));
    spdlog::flush_every(std::chrono::seconds(1));
    spdlog::info("VeloPlay {} starting", VELOPLAY_VERSION);
}

std::string fontPath(const wchar_t* fileName) {
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const std::filesystem::path path = std::filesystem::path(windows) / L"Fonts" / fileName;
    const std::u8string utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

std::filesystem::path executablePath() {
    wchar_t buffer[32768] = {};
    const DWORD length = GetModuleFileNameW(nullptr, buffer, ARRAYSIZE(buffer));
    return std::filesystem::path(std::wstring(buffer, length));
}

/// Refreshes per second of the screen a window is on; 0 if Windows does not say.
double refreshRateOf(HWND window) {
    MONITORINFOEXW monitor{};
    monitor.cbSize = sizeof(monitor);
    if (!GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) return 0.0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0.0;
    // 0 and 1 stand for "whatever the hardware does by default".
    return mode.dmDisplayFrequency > 1 ? static_cast<double>(mode.dmDisplayFrequency) : 0.0;
}

bool reducedMotion() {
    BOOL animations = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
    return animations == FALSE;
}

float scaleForDpi(UINT dpi) { return static_cast<float>(dpi) / 96.0f; }

} // namespace

Application::Application() = default;

Application::~Application() {
    controller_.reset(); // stops background threads before the window goes away
    if (imguiReady_) {
        if (dx11Ready_) ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    if (window_) DestroyWindow(window_);
    UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
}

std::optional<int> Application::runCommand() {
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return std::nullopt;
    bool install = false;
    bool remove = false;
    bool quiet = false;
    for (int i = 1; i < count; ++i) {
        const std::wstring_view argument(arguments[i]);
        if (argument == L"--install") install = true;
        if (argument == L"--uninstall") remove = true;
        if (argument == L"--quiet") quiet = true;
    }
    LocalFree(arguments);
    if (remove) {
        // What Windows runs when the user removes VeloPlay under Settings > Apps.
        const bool german = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN;
        const ui::Strings& t = ui::strings(german ? ui::Language::German : ui::Language::English);
        return uninstall(executablePath(), quiet, kWindowClass, toWide(t.uninstallRunning), toWide(t.uninstallDone));
    }
    if (install) {
        // Installing without the window, for scripts and for the check after each build.
        return createInstaller(executablePath())->install() ? 0 : 1;
    }
    return std::nullopt;
}

void Application::startSuccessor(const std::filesystem::path& exe) {
    // After an update the program starts itself again. That used to happen at
    // the very end, when this window was gone: by then Windows had brought
    // some other program to the front, and a program started from the
    // background may not push itself in front of it. The new window opened
    // behind everything. So the new instance is started now, while this
    // window is the one in front, and is allowed by name to take the front.
    // It waits until this one has ended before it touches ports and files.
    successorProgram_ = exe;
    const std::filesystem::path folder = exe.parent_path();
    const std::wstring arguments = L"--after " + std::to_wstring(GetCurrentProcessId());
    SHELLEXECUTEINFOW start{};
    start.cbSize = sizeof(start);
    start.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    start.lpVerb = L"open";
    start.lpFile = exe.c_str();
    start.lpParameters = arguments.c_str();
    start.lpDirectory = folder.c_str();
    start.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&start)) {
        spdlog::warn("Restart: the new instance could not be started now ({}); trying again at the end", GetLastError());
        return;
    }
    successorStarted_ = true;
    if (start.hProcess) {
        AllowSetForegroundWindow(GetProcessId(start.hProcess));
        CloseHandle(start.hProcess);
    }
}

void Application::waitForPredecessor() {
    // "--after <process id>": started by an instance that is about to end.
    int count = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!arguments) return;
    // For the check on GitHub's machine: the path after a lost graphics device, without losing one.
    for (int i = 1; i < count; ++i) {
        if (std::wstring_view(arguments[i]) == L"--test-device-loss") testDeviceLoss_ = true;
        // To try the way through Sony's servers without leaving home.
        if (std::wstring_view(arguments[i]) == L"--through-sony") throughSony_ = true;
    }
    for (int i = 1; i < count; ++i) {
        if (std::wstring_view(arguments[i]) != L"--after") continue;
        startedAfter_ = true;
        const DWORD id = i + 1 < count ? static_cast<DWORD>(wcstoul(arguments[i + 1], nullptr, 10)) : 0;
        if (id == 0) break;
        // It holds the log file, the settings and the ports until it has ended. If it never does, go on anyway.
        if (const HANDLE before = OpenProcess(SYNCHRONIZE, FALSE, id)) {
            WaitForSingleObject(before, 15'000);
            CloseHandle(before);
        }
        break;
    }
    LocalFree(arguments);
}

int Application::run() {
    if (const std::optional<int> done = runCommand()) return *done;
    waitForPredecessor();
    // The embedded browser needs this thread to be a single-threaded COM apartment.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    (void)com;
    const std::filesystem::path dataDirectory = defaultConfigDirectory();
    setUpLogging(dataDirectory);

    if (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN) systemLanguage_ = "de";

    if (!createWindow()) {
        MessageBoxW(nullptr, L"VeloPlay could not create its window.", L"VeloPlay", MB_ICONERROR);
        return 1;
    }

    std::string error;
    if (!device_.create(window_, error)) {
        spdlog::error("Direct3D 11 is not available: {}", error);
        MessageBoxW(nullptr,
                    L"VeloPlay needs a graphics card with Direct3D 11.\n\n"
                    L"Updating the graphics driver usually fixes this.",
                    L"VeloPlay", MB_ICONERROR);
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr; // window layout is fixed, nothing to remember
    ImGui_ImplWin32_Init(window_);
    ImGui_ImplDX11_Init(device_.device(), device_.context());
    imguiReady_ = true;
    dx11Ready_ = true;

    protector_ = createDpapiProtector();
    AppController::Options options;
    options.configPath = dataDirectory / "config.json";
    options.protector = protector_.get();
    options.controllerBackend = input::createSdlControllerBackend();
    options.openHelp = [this](std::string_view topic) { openHelp(topic); };
    options.openAddress = [this](const std::string& url) {
        ShellExecuteW(window_, L"open", toWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    // Asked for from inside the drawing of a frame; resizing the window there would pull the
    // picture away under the drawing. It is done before the next frame instead.
    options.windowAction = [this](WindowAction action) { pendingWindowAction_ = action; };
    options.sessionFactory = session::chiakiSessionFactory();
    // The controller is read every two milliseconds on a thread of its own.
    // Read once per picture, a press waited up to a whole refresh of the
    // screen before it was even sent.
    options.inputInterval = std::chrono::microseconds{2000};

    // Account lookup: Sony's sign-in page in a window of our own. What the
    // embedded browser stores (cookies of the sign-in) is deleted at every
    // start and when the program ends.
    loginDataDirectory_ = dataDirectory / "signin";
    std::error_code ignored;
    std::filesystem::remove_all(loginDataDirectory_, ignored);
    options.showLogin = [this](const std::string& url) {
        const bool german = ui::resolveLanguage(controller_->config().language, systemLanguage_) == ui::Language::German;
        const std::wstring title = toWide(ui::strings(german ? ui::Language::German : ui::Language::English).accountSignIn);
        return loginWindow_.open(
            window_, url, protocol::psnRedirectPrefix(), loginDataDirectory_, title,
            [this](std::optional<std::string> redirect) {
                if (redirect) controller_->submitAccountRedirect(*redirect);
                else controller_->accountLoginClosed();
            },
            [this, url] {
                // No embedded browser on this PC: use the normal one. The
                // dialog explains how to bring the address back by hand.
                ShellExecuteW(window_, L"open", toWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            });
    };

    // Picture and sound. Without a working renderer the stream is still
    // received and the controller still works; the window just stays dark.
    std::string renderError;
    videoReady_ = videoRenderer_.create(device_.device(), device_.context(), renderError);
    if (!videoReady_) spdlog::error("Video output is not available: {}", renderError);
    // Made before the sound output, which hands it every block of sound from its first one on.
    recorder_ = std::make_unique<record::VideoRecorder>();
    audioOutput_ = std::make_unique<audio::WasapiOutput>();
    // What a recording hears is what goes to the speakers, after the delay that matches the picture.
    audioOutput_->setTap([recorder = recorder_.get()](std::span<const std::int16_t> sound, int rate, int channels) {
        recorder->addSound(sound, rate, channels);
    });
    microphone_ = std::make_unique<audio::WasapiInput>();
    options.microphone = microphone_.get();
    // The DualSense shows up as a sound device of its own when it is on a cable.
    hapticsOutput_ = std::make_unique<audio::WasapiOutput>(
        std::vector<std::wstring>{L"Wireless Controller", L"DualSense"});
    if (videoReady_) {
        pipeline_ = std::make_unique<media::MediaPipeline>(
            media::MediaPipeline::Options{device_.device(), audioOutput_.get(), hapticsOutput_.get()});
        options.mediaSink = pipeline_.get();
    }
    // Self-update: the client asks GitHub for the latest release and replaces
    // its own files in the folder it was started from.
    http_ = update::createWinHttpClient("VeloPlay/" VELOPLAY_VERSION);
    options.http = http_.get();
    options.installDir = executablePath().parent_path();
    options.restart = [this] {
        restartRequested_ = true;
        startSuccessor(executablePath());
        PostMessageW(window_, WM_CLOSE, 0, 0);
    };
    // One screenshot at a time: the next one is taken once the last is written.
    options.takeScreenshot = [this] {
        if (!saving_.valid()) grabber_.request();
    };
    {
        PWSTR raw = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
            picturesFolder_ = std::filesystem::path(raw) / L"VeloPlay";
        }
        if (raw) CoTaskMemFree(raw);
    }
    options.picturesFolder = picturesFolder_;
    {
        PWSTR raw = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
            videosFolder_ = std::filesystem::path(raw) / L"VeloPlay";
        }
        if (raw) CoTaskMemFree(raw);
    }
    // Which pictures of a video were computed: beside the settings, so the folder of videos stays tidy.
    frameLogFolder_ = dataDirectory / "recordings";
    recordAvailable_ = !videosFolder_.empty() && record::VideoRecorder::available(&recordMissing_);
    if (!recordAvailable_) {
        if (recordMissing_.empty()) recordMissing_ = "Windows did not name a folder for videos";
        spdlog::info("Recording is not available: {}", recordMissing_);
    }
    options.videosFolder = videosFolder_;
    captureScan_ = std::jthread([this](std::stop_token stop) {
        while (!stop.stop_requested()) {
            std::vector<CaptureDevice> found = capture::listDevices();
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
            for (int slice = 0; slice < 50 && !stop.stop_requested(); ++slice) Sleep(100);
        }
    });
    // Asked once per connection attempt: the adapters may have changed (a VPN switched on).
    options.isAway = [](std::uint32_t address) { return !net::isAtHome(address, net::localSubnets()); };
    // Through Sony's servers only where this build carries the parts for it.
    if (session::psnAvailable()) options.psnConsoles = [](const std::string& token) { return session::listPsnConsoles(token); };
    options.alwaysThroughSony = throughSony_;
    if (throughSony_) spdlog::info("Started with --through-sony: consoles are reached through Sony's servers");
    options.captureDevices = [this] {
        std::lock_guard lock(captureMutex_);
        return captureDevices_;
    };
    // Discord's card while a game is streamed; the pipe finds Discord's desktop app if it runs.
    options.discordPipe = [] { return discord::createPipe(); };
    options.processId = GetCurrentProcessId();
    options.systemLanguage = systemLanguage_;
    options.captureFactory = [] { return std::unique_ptr<session::ISession>(std::make_unique<capture::CaptureSession>()); };
    options.toggleRecording = [this] { recordToggle_ = true; };
    options.recording = [this] {
        UiModel::RecordingView view;
        view.available = recordAvailable_;
        view.missing = recordMissing_;
        view.starting = recordStarting_.valid();
        view.running = !view.starting && !recordStopping_.valid() && recorder_->running();
        if (view.running) view.seconds = recorder_->seconds();
        return view;
    };
    options.openFolder = [](const std::filesystem::path& folder) {
        std::error_code ignored;
        std::filesystem::create_directories(folder, ignored);
        ShellExecuteW(nullptr, L"explore", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    // Installing on this PC: the installed program takes over as the new instance does after an update.
    installer_ = createInstaller(executablePath());
    options.installer = installer_.get();
    options.startInstalled = [this](const std::filesystem::path& program) {
        restartRequested_ = true;
        startSuccessor(program);
        PostMessageW(window_, WM_CLOSE, 0, 0);
    };
    refreshInstallation(executablePath()); // an update changed the version Windows lists
    controller_ = std::make_unique<AppController>(std::move(options));
    controller_->start();

    ui_ = std::make_unique<ui::UiManager>(*controller_);
    fonts_ = ui::loadFonts(fontPath(L"segoeui.ttf").c_str(), fontPath(L"seguisb.ttf").c_str());
    applyScale(scaleForDpi(GetDpiForWindow(window_)));

    // "Not responding" in the title bar means this thread did not ask Windows
    // for messages for five seconds. The log then names the step it was in
    // and shows where the thread waits, down to the library.
    threadStack_.watchCurrentThread();
    stallWatch_.start(std::chrono::milliseconds{1500},
                      [this, window = window_, stacks = 0](const diag::StallReport& report) mutable {
                          if (report.step == kModalStep) {
                              // Not a hang, but worth a line: it explains a picture that stood still.
                              if (report.finished) {
                                  spdlog::info("Windows ran a menu or move loop inside the window for {} ms",
                                               report.duration.count());
                              }
                              return;
                          }
                          if (report.finished) {
                              spdlog::warn("The window was stuck for {} ms in: {}", report.duration.count(), report.step);
                              stacks = 0;
                              return;
                          }
                          spdlog::warn("The window has been stuck for {} ms in: {} (minimised: {}, in front: {})",
                                       report.duration.count(), report.step, IsIconic(window) ? "yes" : "no",
                                       GetForegroundWindow() == window ? "yes" : "no");
                          // The first looks are the ones that matter; a long hang would only repeat them.
                          if (stacks++ >= 3) return;
                          for (const std::string& line : threadStack_.capture()) spdlog::warn("    at {}", line);
                      });

    ShowWindow(window_, SW_SHOWDEFAULT);
    UpdateWindow(window_);
    // Full screen is how it starts, unless the user left it as a window the last time.
    if (controller_->config().window.fullscreen && !IsIconic(window_)) toggleFullscreen(false);
    if (startedAfter_) {
        // Started by the instance before it, after an update: the user is looking at where that one was.
        SetForegroundWindow(window_);
        const bool inFront = GetForegroundWindow() == window_;
        if (!inFront) {
            // Windows did not let it take the front. Above the other windows it can still be.
            SetWindowPos(window_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SetWindowPos(window_, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        spdlog::info("Window: started after an update, {}", inFront ? "in front" : "placed above the other windows");
    }

    // The second watch asks Windows itself. A window can count as "not
    // responding" while its loop runs (issue 3); the first watch cannot see that.
    hangProbe_ = std::jthread([this, window = window_](std::stop_token stop) { watchForHang(window, stop); });

    if (testDeviceLoss_) testLossAt_ = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    MSG message{};
    while (message.message != WM_QUIT) {
        diag::loopTurned();
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            messageTrace_.note(message.message);
            diag::setStep("handling a window message");
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }
        if (deviceLost_) {
            // The window keeps answering while the card comes back; the stream goes on meanwhile.
            const auto now = std::chrono::steady_clock::now();
            if (!deviceGivenUp_ && now >= nextRecovery_ && !recoverDevice(now)) {
                // A driver that is being updated or restarted needs some seconds; past that it is not coming.
                if (now - lostSince_ >= std::chrono::seconds(20)) deviceGivenUp_ = true;
                else nextRecovery_ = now + std::chrono::seconds(1);
            }
            if (deviceGivenUp_) {
                spdlog::error("Graphics: no new device after {} attempt(s); VeloPlay ends", recoveryAttempts_);
                break;
            }
            if (deviceLost_) MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
            continue;
        }
        if (testLossAt_ != std::chrono::steady_clock::time_point{} && std::chrono::steady_clock::now() >= testLossAt_) {
            testLossAt_ = {};
            markDeviceLost("a test asked for it");
            continue;
        }
        frame();
    }

    if (std::exchange(timerRaised_, false)) timeEndPeriod(1);
    // Every way out closes a recording so the file plays: quitting, an update, a lost graphics device.
    if (recordStarting_.valid()) (void)recordStarting_.get();
    if (recordStopping_.valid()) recordingStopped(recordStopping_.get());
    if (recorder_ && recorder_->running()) recordingStopped(recorder_->stop());
    // From here on the window stops answering on purpose.
    hangProbe_.request_stop();
    if (hangProbe_.joinable()) hangProbe_.join();
    stallWatch_.stop();

    loginWindow_.close();
    {
        std::error_code ignored;
        std::filesystem::remove_all(loginDataDirectory_, ignored);
    }
    if (restartRequested_) {
        // The new instance waits for this one to end; it finds ports and files free then.
        ui_.reset();
        controller_.reset();
        if (!successorStarted_) {
            const std::filesystem::path exe = successorProgram_.empty() ? executablePath() : successorProgram_;
            ShellExecuteW(nullptr, L"open", exe.c_str(), L"--after 0", exe.parent_path().c_str(), SW_SHOWNORMAL);
        }
        return 0;
    }

    if (deviceGivenUp_) {
        MessageBoxW(window_,
                    L"The graphics driver stopped responding or was updated.\n\nPlease start VeloPlay again.",
                    L"VeloPlay", MB_ICONWARNING);
        return 1;
    }
    return static_cast<int>(message.wParam);
}

bool Application::createWindow() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &Application::windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // Resource 1 is the VeloPlay icon (src/app/veloplay.rc). Windows picks the size that fits.
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                                        LR_DEFAULTCOLOR));
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&windowClass)) return false;

    // Size the window for the monitor it will open on.
    const float scale = scaleForDpi(GetDpiForSystem());
    window_ = CreateWindowExW(0, kWindowClass, L"VeloPlay", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                              static_cast<int>(kDefaultWidth * scale), static_cast<int>(kDefaultHeight * scale), nullptr,
                              nullptr, instance, this);
    if (!window_) return false;

    // Dark frame to match the content (Windows 10 2004 and later).
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    // The window has no title bar from Windows (see WM_NCCALCSIZE); it draws
    // a bar of its own. Its size is worked out again so that holds before it is shown.
    SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    return true;
}

void Application::applyScale(float scale) {
    scale_ = scale;
    if (ui_) ui_->configure(fonts_, scale_, systemLanguage_);
}

void Application::frame() {
    // A minimised window has nothing to draw. Background services keep running.
    if (IsIconic(window_)) {
        controller_->update();
        diag::setStep("collecting what to show");
        controller_->fillModel(model_);
        logStreamSummary(std::chrono::steady_clock::now());
        diag::setStep("idle, minimised");
        Sleep(50);
        return;
    }

    if (const std::optional<WindowAction> action = std::exchange(pendingWindowAction_, std::nullopt)) {
        applyWindowAction(*action);
        if (!window_ || IsIconic(window_)) return;
    }
    updateTaskbarPeek(std::chrono::steady_clock::now());

    // The controller decides which notes show over the picture; what the decoder threw away it learns here.
    if (pipeline_) controller_->noteDroppedFrames(pipeline_->stats().droppedFrames);
    controller_->notePictureTiming(pacer_.unevenness(), std::chrono::duration<double, std::milli>(pacer_.reserve()).count(),
                                   insideMs_, std::chrono::duration<double, std::milli>(pacer_.addedDelay()).count());
    controller_->update();
    diag::setStep("collecting what to show");
    controller_->fillModel(model_);
    model_.gpuName = device_.adapterName();
    model_.gpuVendor = device_.adapterVendor() == 0x10DE   ? UiModel::GpuVendor::Nvidia
                       : device_.adapterVendor() == 0x1002 ? UiModel::GpuVendor::Amd
                       : device_.adapterVendor() == 0x8086 ? UiModel::GpuVendor::Intel
                                                           : UiModel::GpuVendor::Other;
    model_.reducedMotion = reducedMotion();
    model_.rtxVideoAvailable = videoRenderer_.rtxAvailable();
    // AMD FRC only on an AMD card: on any other the runtime is missing, and it would only be searched for.
    videoRenderer_.useAmdFrc(controller_->config().graphics.amdFrc && device_.adapterVendor() == 0x1002);
    model_.amdFrcActive = videoRenderer_.amdFrcActive();
    model_.amdFrcError = videoRenderer_.amdFrcError();
    model_.ownWindowBar = true;
    model_.windowFullscreen = fullscreen_;
    model_.windowMaximized = IsZoomed(window_) != FALSE;
    {
        SYSTEMTIME local{};
        GetLocalTime(&local);
        char clock[8] = {};
        std::snprintf(clock, sizeof(clock), "%02u:%02u", static_cast<unsigned>(local.wHour), static_cast<unsigned>(local.wMinute));
        model_.clock = clock;
    }
    // The window can be moved to another screen at any time; once a second is often enough.
    if (const auto now = std::chrono::steady_clock::now(); now >= nextDisplayCheck_) {
        nextDisplayCheck_ = now + std::chrono::seconds(1);
        displayHz_ = refreshRateOf(window_);
    }
    model_.displayHz = displayHz_;

    const bool streaming = model_.state == AppState::Streaming;
    // Windows wakes sleeping threads only every 15 ms unless asked for more.
    // While a stream runs, the input thread needs its two milliseconds.
    if (streaming != timerRaised_) {
        if (streaming) timeBeginPeriod(1);
        else timeEndPeriod(1);
        timerRaised_ = streaming;
    }
    const AppConfig& config = controller_->config();
    if (pipeline_) {
        const auto now = std::chrono::steady_clock::now();
        pipeline_->setHapticsEnabled(config.controller.rumble);

        if (!streaming) {
            forgetPictures();
        } else if (model_.stream && now >= nextRateSample_) {
            // Once a second: is it a 30 or a 60 fps stream that arrives?
            nextRateSample_ = now + std::chrono::seconds(1);
            inputRate_.addSample(model_.stream->streamFps);
        }
        // What arrives decides what is made of it: the factor for frame
        // generation follows the measured rate when it is set to automatic.
        // A game at 30 fps in a 60 fps stream counts as 30: the console sends each of its pictures twice.
        const bool halved = streaming && inputRate_.fps() == 60 && cadence_.halved();
        const int gameFps = halved ? 30 : inputRate_.fps();
        const int wanted = media::frameGenerationFactorFor(config.graphics.frameGenerationFactor, gameFps, displayHz_);
        const bool available = videoRenderer_.frameGenerationAvailable();
        // Without pictures to compute in between, the repeats may as well be shown.
        skipRepeats_ = halved && available && wanted > 1;
        // The refresh measured below: the pacer waits half of one for pictures, so none is lost to one coming
        // a moment after a refresh.
        pacer_.configure(available ? wanted : 1, std::chrono::microseconds(1'000'000 / (skipRepeats_ ? 30 : inputRate_.fps())),
                         std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::duration<double, std::milli>(refreshMs_)));

        // A console on Wi-Fi delivers in bursts with gaps of tens of milliseconds
        // between them; chiaki-ng keeps about 100 ms of sound for that reason.
        // "Automatic" finds the amount while playing: a gap in the sound raises it.
        // Pictures held back for frame generation hold the sound back as well.
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
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ui_->feedNavigation(model_);
    ImGui::NewFrame();
    ui_->draw(model_);
    ImGui::Render();
    // Keys and mouse go to the game while its picture is all there is on
    // screen and this window is the one in front; a menu, the settings or
    // another window take them back.
    setGameInput(streaming && config.controller.keyboardMouse.enabled && ui_->pictureOwnsInput() &&
                 !controller_->capturingKey() && GetForegroundWindow() == window_);

    // Waits for the display here, where no lock is held, instead of in present().
    diag::setStep("waiting for the display");
    device_.waitForFrame(100);

    // The picture is fetched after the wait, not before it: whatever arrived
    // while waiting goes out with this refresh instead of the next one.
    render::VideoRenderer::Output output;
    bool freshPicture = false; // the picture differs from the one the refresh before showed
    if (pipeline_ && streaming) {
        diag::setStep("taking over a picture");
        const auto now = std::chrono::steady_clock::now();
        videoRenderer_.wantGeneration(pacer_.factor() > 1);
        // How far apart the screen's refreshes are, taken from how often this
        // point is reached. Half of that is how early a picture may go out,
        // so each one goes with the refresh nearest to its moment.
        if (lastRefresh_ != std::chrono::steady_clock::time_point{}) {
            const double gap = std::chrono::duration<double, std::milli>(now - lastRefresh_).count();
            if (gap > 1.0 && gap < 50.0) refreshMs_ = refreshMs_ == 0.0 ? gap : refreshMs_ * 0.9 + gap * 0.1;
        }
        lastRefresh_ = now;
        const auto slack = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(refreshMs_ * 0.5));
        for (media::VideoFrame& picture : pipeline_->takeFrames()) waiting_.push_back(std::move(picture));
        // More waiting than a few refreshes' worth (the window was held up, the screen is slower than
        // the stream): the oldest go, so the picture does not trail ever further behind.
        while (waiting_.size() > 8) {
            waiting_.pop_front();
            ++overflowPictures_;
        }
        while (!waiting_.empty()) {
            // The repeats of a game that runs at half the stream's rate are compared and left out.
            const bool repeat = skipRepeats_ && cadence_.isRepeat(videoRenderer_.submitted() + 1);
            // A real picture goes to the renderer once it is needed; repeats are only measured, in turn.
            if (!repeat && !pacer_.wantsNext(now, slack) && waiting_.size() <= 4) break;
            const media::VideoFrame picture = std::move(waiting_.front());
            waiting_.pop_front();
            const bool taken = videoRenderer_.submit(picture, !repeat);
            while (const auto change = videoRenderer_.takeChange()) cadence_.add(change->picture, change->mean);
            if (cadence_.halved() != loggedHalved_) {
                loggedHalved_ = cadence_.halved();
                const double* lately = cadence_.recent();
                spdlog::info("Game rate: {} (change of the last three pictures: {:.2f}, {:.2f}, {:.2f} levels)",
                             loggedHalved_ ? "30 fps, every other picture repeats" : "as the stream", lately[0],
                             lately[1], lately[2]);
            }
            if (!taken || repeat) continue;
            // Laid out by when the decoder finished, not by when this loop came around.
            const bool stamped = picture.decodedAt != std::chrono::steady_clock::time_point{};
            pacer_.onFrame(videoRenderer_.newestId(), stamped ? picture.decodedAt : now);
            arrivals_[arrivalsNext_++ % arrivals_.size()] = {videoRenderer_.newestId(), picture.receivedAt};
            // Which method computes the pictures in between, once per stream and whenever it changes.
            if (videoRenderer_.frameGenerationAvailable() && loggedGenerator_ != videoRenderer_.frameGeneratorName()) {
                loggedGenerator_ = videoRenderer_.frameGeneratorName();
                const std::string why = videoRenderer_.cardMotionError();
                spdlog::info("Frame generation: {}{}{}", loggedGenerator_,
                             why.empty() ? "" : " (the graphics card's own motion search is not used: ", why.empty() ? "" : why + ")");
            }
            if (!videoRenderer_.frameGenerationAvailable() && !generatorFailureLogged_) {
                generatorFailureLogged_ = true;
                spdlog::warn("Frame generation is not available: {}", videoRenderer_.frameGeneratorError());
            }
        }
        output.choice = pacer_.choose(now, slack);
        output.upscale = config.graphics.superResolutionLevel > 0;
        output.rtx = config.graphics.superResolutionLevel >= 2;
        if (output.rtx && !videoRenderer_.rtxAvailable() && !rtxFailureLogged_ && !videoRenderer_.rtxError().empty()) {
            rtxFailureLogged_ = true;
            spdlog::warn("RTX Video Super Resolution is not used: {}", videoRenderer_.rtxError());
        }
        output.sharpness = config.graphics.sharpness;
        output.deband = config.graphics.deband;
        output.look = config.graphics.look;
        output.lookStrength = config.graphics.lookStrength;
        // A picture counts as shown when it differs from the one before, real or computed.
        if (output.choice.valid && output.choice == lastChoice_ && videoRenderer_.hasPicture()) ++repeatedRefreshes_;
        if (output.choice.valid && output.choice.step == 0 && output.choice.base > lastRealShown_) {
            if (lastRealShown_ > 0 && output.choice.base > lastRealShown_ + 1) skippedPictures_ += output.choice.base - lastRealShown_ - 1;
            lastRealShown_ = output.choice.base;
        }
        if (output.choice.valid && !(output.choice == lastChoice_)) {
            freshPicture = true;
            lastChoice_ = output.choice;
            shownMeter_.addVideo(0, 0);
            // How long a real picture spent in this PC, from its data arriving to being drawn.
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

    const ImVec4& background = ui::palette().background;
    // Black bars around the picture, the usual background otherwise.
    const float clear[4] = {showVideo ? 0.0f : background.x, showVideo ? 0.0f : background.y,
                            showVideo ? 0.0f : background.z, 1.0f};
    diag::setStep("drawing");
    device_.beginFrame(clear);
    if (showVideo) videoRenderer_.draw(device_.width(), device_.height(), output);
    // Before the overlays: a screenshot holds the game, not VeloPlay's display or notes.
    if (showVideo && grabber_.busy()) {
        const UINT shownWidth = static_cast<UINT>(std::max(0, videoRenderer_.shownWidth()));
        const UINT shownHeight = static_cast<UINT>(std::max(0, videoRenderer_.shownHeight()));
        D3D11_BOX region{};
        region.left = (device_.width() - std::min(shownWidth, device_.width())) / 2;
        region.top = (device_.height() - std::min(shownHeight, device_.height())) / 2;
        region.right = region.left + std::min(shownWidth, device_.width());
        region.bottom = region.top + std::min(shownHeight, device_.height());
        grabber_.capture(device_.device(), device_.context(), device_.backBuffer().Get(), region);
    }
    handleRecording(showVideo,
                    !freshPicture ? media::ShownKind::Repeated
                                  : (output.choice.step == 0 ? media::ShownKind::Real : media::ShownKind::Computed),
                    std::chrono::steady_clock::now());
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    diag::setStep("presenting");
    if (!device_.present(streaming ? config.graphics.vsync : true)) markDeviceLost(device_.lostReason());
    collectScreenshot();
    diag::setStep("idle");

    // The dashboard does not need 60 redraws per second while another window
    // has focus. A running stream does.
    if (!streaming && GetForegroundWindow() != window_) Sleep(40);
}

void Application::forgetPictures() {
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

void Application::markDeviceLost(const std::string& why) {
    const auto now = std::chrono::steady_clock::now();
    spdlog::warn("Graphics: the device was lost ({}); it is made anew", why.empty() ? "no reason given" : why);
    std::erase_if(losses_, [&](std::chrono::steady_clock::time_point at) { return now - at > std::chrono::minutes(1); });
    losses_.push_back(now);
    // A card that falls over again and again right after each new start would only flicker on.
    if (losses_.size() > 3) {
        spdlog::error("Graphics: lost {} times within a minute", losses_.size());
        deviceGivenUp_ = true;
    }
    deviceLost_ = true;
    lostSince_ = now;
    nextRecovery_ = now + std::chrono::milliseconds(200); // a moment for the driver to come back
    recoveryAttempts_ = 0;
    vendorBeforeLoss_ = device_.adapterVendor();
}

bool Application::recoverDevice(std::chrono::steady_clock::time_point now) {
    diag::setStep("making the graphics device anew");
    ++recoveryAttempts_;
    // A recording ends with a file that plays: its copies of the picture lived on the old device.
    if (recordStarting_.valid()) {
        const Result<bool> started = recordStarting_.get();
        if (!started) spdlog::warn("Recording did not start: {}", started.error().message);
    }
    if (!recordStopping_.valid() && recorder_ && recorder_->running()) stopRecording("the graphics device was lost");
    recordStream_.reset();
    grabber_ = render::FrameGrabber{};
    // Everything made on the old device goes before the device itself, the swap chain above all:
    // a window takes a new one only once the old one is gone.
    videoRenderer_.reset();
    if (pipeline_) pipeline_->setDevice(nullptr);
    if (dx11Ready_) {
        ImGui_ImplDX11_Shutdown();
        dx11Ready_ = false;
    }
    device_.destroy();

    std::string error;
    if (!device_.create(window_, error)) {
        spdlog::warn("Graphics: attempt {} found no device: {}", recoveryAttempts_, error);
        return false;
    }
    // Right after a driver restart Windows can offer only its software renderer for a moment.
    // Taking that would leave the stream on the processor for the rest of the run.
    // During a driver update it may also be a basic adapter of Windows; either way not the card from before.
    if (vendorBeforeLoss_ != 0 && device_.adapterVendor() != vendorBeforeLoss_ && now - lostSince_ < std::chrono::seconds(10)) {
        spdlog::info("Graphics: only {} so far; waiting for the graphics card", device_.adapterName());
        device_.destroy();
        return false;
    }
    ImGui_ImplDX11_Init(device_.device(), device_.context());
    dx11Ready_ = true;
    std::string renderError;
    if (!videoRenderer_.create(device_.device(), device_.context(), renderError)) {
        spdlog::warn("Graphics: video output on the new device failed: {}", renderError);
        // Without the renderer there is no picture; it was there before, so this is no recovery yet.
        if (videoReady_) return false;
    }
    if (pipeline_) pipeline_->setDevice(device_.device());
    forgetPictures();
    generatorFailureLogged_ = false;
    rtxFailureLogged_ = false;
    deviceLost_ = false;
    spdlog::info("Graphics: device made anew on {} after {} attempt(s), {} ms after it was lost", device_.adapterName(),
                 recoveryAttempts_,
                 std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - lostSince_).count());
    controller_->graphicsRestored(std::chrono::steady_clock::now());
    return true;
}

void Application::handleRecording(bool showVideo, media::ShownKind kind, std::chrono::steady_clock::time_point now) {
    using namespace std::chrono_literals;
    if (recordStopping_.valid() && recordStopping_.wait_for(0s) == std::future_status::ready) {
        recordingStopped(recordStopping_.get());
    }
    if (recordStarting_.valid() && recordStarting_.wait_for(0s) == std::future_status::ready) {
        const Result<bool> started = recordStarting_.get();
        if (!started) {
            spdlog::warn("Recording did not start: {}", started.error().message);
            controller_->captureFinished(false, {}, started.error().message, true);
        }
    }
    const bool busy = recordStarting_.valid() || recordStopping_.valid();
    const bool running = !busy && recorder_->running();

    // The part of the screen with the picture, without the black bars; H.264 wants even sizes.
    const UINT shownWidth = std::min(static_cast<UINT>(std::max(0, videoRenderer_.shownWidth())), device_.width());
    const UINT shownHeight = std::min(static_cast<UINT>(std::max(0, videoRenderer_.shownHeight())), device_.height());
    const UINT left = (device_.width() - shownWidth) / 2;
    const UINT top = (device_.height() - shownHeight) / 2;
    const int width = static_cast<int>(shownWidth & ~1u);
    const int height = static_cast<int>(shownHeight & ~1u);

    if (std::exchange(recordToggle_, false) && !busy) {
        if (running) {
            stopRecording("asked to");
            return;
        }
        if (!recordAvailable_ || !showVideo) return;
        std::time_t clock = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &clock);
        const Result<std::filesystem::path> file = media::freeCapturePath(videosFolder_, local, "mp4");
        if (!file) {
            controller_->captureFinished(false, {}, file.error().message, true);
            return;
        }
        recordFile_ = file.value();
        recordStream_.reset();
        // At most 60 pictures per second: screens beyond that would only make bigger files and cost
        // the window more copying, and not every encoder takes higher rates.
        const int fps = std::min(60, static_cast<int>(std::lround(displayHz_ > 1.0 ? displayHz_ : 60.0)));
        recordFps_ = fps;
        lastRecorded_ = {};
        // Opening the encoder takes a moment; the window goes on drawing meanwhile.
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
    // A file keeps one size. A window that changes it (F11, resizing) ends the recording.
    if (width != recorder_->width() || height != recorder_->height()) {
        stopRecording("the window changed its size");
        return;
    }
    // On a screen faster than the recording, refreshes in between are left out. Allowed to come a
    // quarter of a picture early, so a 60 Hz screen's slightly uneven refreshes all go in.
    const auto interval = std::chrono::duration<double>(0.75 / recordFps_);
    if (lastRecorded_ == std::chrono::steady_clock::time_point{} || now - lastRecorded_ >= interval) {
        if (recordStream_.capture(device_.device(), device_.context(), device_.backBuffer().Get(), left, top, width,
                                  height, kind, now)) {
            lastRecorded_ = now;
        }
    }
    while (auto picture = recordStream_.take(device_.context(), recorder_->buffer())) {
        recorder_->addPicture(std::move(picture->bgra), picture->kind, picture->at);
    }
}

void Application::stopRecording(const char* why) {
    spdlog::info("Recording ends: {}", why);
    recordStream_.reset();
    // Writing what is left and closing the file can take a moment; the window goes on.
    recordStopping_ = std::async(std::launch::async, [recorder = recorder_.get()] { return recorder->stop(); });
}

void Application::recordingStopped(Result<record::RecordingSummary> outcome) {
    if (!outcome) {
        spdlog::warn("Recording failed: {}", outcome.error().message);
        if (controller_) controller_->captureFinished(false, {}, outcome.error().message, true);
        return;
    }
    const record::RecordingSummary& summary = outcome.value();
    // One letter per picture in the file: real, computed or repeated.
    std::error_code ignored;
    std::filesystem::create_directories(frameLogFolder_, ignored);
    std::filesystem::path log = frameLogFolder_ / summary.file.filename();
    log.replace_extension(L".frames.txt");
    if (std::FILE* out = _wfopen(log.c_str(), L"wb")) {
        const std::string text = summary.log.text();
        std::fwrite(text.data(), 1, text.size(), out);
        std::fclose(out);
    }
    const std::u8string where = videosFolder_.u8string();
    if (controller_) controller_->captureFinished(true, std::string(where.begin(), where.end()), {}, true);
}

void Application::collectScreenshot() {
    if (saving_.valid() && saving_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        const Saved saved = saving_.get();
        controller_->captureFinished(saved.ok, saved.where, saved.detail);
    }
    if (const char* why = grabber_.failure(); why && *why && !grabber_.busy() && !saving_.valid()) {
        controller_->captureFinished(false, {}, why);
        grabber_ = render::FrameGrabber{};
        return;
    }
    std::optional<media::Image> image = grabber_.take(device_.context());
    if (!image) return;
    // Packing a 1080p picture takes a moment; the window must not wait for it.
    saving_ = std::async(std::launch::async, [picture = std::move(*image), folder = picturesFolder_]() -> Saved {
        if (folder.empty()) return {false, {}, "Windows did not name a folder for pictures"};
        const Result<std::vector<std::uint8_t>> png = media::encodePng(picture);
        if (!png) return {false, {}, png.error().message};
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        const Result<std::filesystem::path> file = media::saveCapture(folder, local, "png", png.value());
        if (!file) return {false, {}, file.error().message};
        const std::u8string where = folder.u8string();
        return {true, std::string(where.begin(), where.end()), {}};
    });
}

void Application::watchForHang(HWND window, std::stop_token stop) {
    bool hung = false;
    unsigned long long since = 0;
    unsigned long long lastDump = 0;
    int dumps = 0;
    while (!stop.stop_requested()) {
        for (int slice = 0; slice < 10 && !stop.stop_requested(); ++slice) Sleep(100);
        const bool looksHung = diag::windowLooksHung(window, 1500);
        if (stop.stop_requested()) break;
        const unsigned long long tick = GetTickCount64();
        if (!looksHung) {
            if (hung) spdlog::warn("Windows sees the window responding again after {} ms", tick - since);
            hung = false;
            continue;
        }
        if (!hung) {
            hung = true;
            since = tick;
        } else if (tick - lastDump < 10'000 || dumps >= 6) {
            continue;
        }
        lastDump = tick;
        ++dumps;
        const char* step = diag::currentStep();
        spdlog::warn("Windows treats the window as not responding (for {} ms). Its loop last came around {} ms ago, in: "
                     "{}. Last message handled: 0x{:04x}, {} ms ago, {} in total. Minimised: {}, in front: {}",
                     tick - since, diag::sinceLoopTurned().count(), step ? step : "an unnamed step",
                     messageTrace_.lastMessage.load(), tick - messageTrace_.lastTick.load(), messageTrace_.count.load(),
                     IsIconic(window) ? "yes" : "no", GetForegroundWindow() == window ? "yes" : "no");
        for (const std::string& line : diag::describeWindows(window)) spdlog::warn("    {}", line);
        for (const std::string& line : threadStack_.capture()) spdlog::warn("    at {}", line);
    }
}

void Application::logStreamSummary(std::chrono::steady_clock::time_point now) {
    // A line every ten seconds: enough to see afterwards when a stream went
    // bad and whether the network, the decoder or the display fell behind.
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
                 "{} dropped from the queue, screen {:.0f} Hz{}{}{}",
                 stream.streamFps, stream.outputFps, stream.bitrateMbps,
                 stream.packetLossPercent, media.decodeMs, media.decodeMaxMs, media.decodedFrames, media.droppedFrames,
                 media.failedFrames, pacer_.factor(), insideMs_,
                 std::chrono::duration<double, std::milli>(pacer_.reserve()).count(), std::exchange(repeatedRefreshes_, 0),
                 std::exchange(skippedPictures_, 0), std::exchange(overflowPictures_, 0), displayHz_,
                 GetForegroundWindow() == window_ ? "" : ", window not in front",
                 cadence_.halved() ? ", game at 30 fps (every other picture repeats)" : "",
                 IsIconic(window_) ? ", window minimised" : "");
}

void Application::setGameInput(bool on) {
    if (on == gameInput_ || !window_) return;
    gameInput_ = on;
    // The mouse as it moves, not where its pointer is: a pointer stops at
    // the edge of the screen, a hand does not.
    RAWINPUTDEVICE mouse{};
    mouse.usUsagePage = 0x01; // generic desktop controls
    mouse.usUsage = 0x02;     // mouse
    mouse.dwFlags = on ? 0 : RIDEV_REMOVE;
    mouse.hwndTarget = on ? window_ : nullptr;
    if (!RegisterRawInputDevices(&mouse, 1, sizeof(mouse)) && on) {
        spdlog::warn("The mouse cannot be read directly (error {}); looking around with it will not work", GetLastError());
    }
    ImGuiIO& io = ImGui::GetIO();
    if (on) {
        // The pointer is hidden and kept in the middle of the window, so a click never lands elsewhere.
        RECT client{};
        GetClientRect(window_, &client);
        POINT centre{(client.left + client.right) / 2, (client.top + client.bottom) / 2};
        ClientToScreen(window_, &centre);
        const RECT hold{centre.x, centre.y, centre.x + 1, centre.y + 1};
        ClipCursor(&hold);
        while (ShowCursor(FALSE) >= 0) {}
        io.ConfigFlags |= ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange;
    } else {
        ClipCursor(nullptr);
        while (ShowCursor(TRUE) < 0) {}
        io.ConfigFlags &= ~(ImGuiConfigFlags_NoMouse | ImGuiConfigFlags_NoMouseCursorChange);
        if (controller_) controller_->keyboardMouse().releaseAll();
    }
    spdlog::info("Keys and mouse belong to the {}", on ? "game" : "program");
}

bool Application::routeInput(UINT message, WPARAM wParam, LPARAM lParam) {
    const bool capturing = controller_->capturingKey();
    if (!gameInput_ && !capturing) return false;

    // Which key or mouse button, and did it go down or up?
    int code = 0;
    bool down = false;
    bool system = false; // with Alt held: Windows needs to see these too (Alt+Tab, Alt+F4)
    switch (message) {
        case WM_SYSKEYDOWN: system = true; [[fallthrough]];
        case WM_KEYDOWN: code = static_cast<int>(wParam); down = true; break;
        case WM_SYSKEYUP: system = true; [[fallthrough]];
        case WM_KEYUP: code = static_cast<int>(wParam); break;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: code = VK_LBUTTON; down = true; break;
        case WM_LBUTTONUP: code = VK_LBUTTON; break;
        case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: code = VK_RBUTTON; down = true; break;
        case WM_RBUTTONUP: code = VK_RBUTTON; break;
        case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: code = VK_MBUTTON; down = true; break;
        case WM_MBUTTONUP: code = VK_MBUTTON; break;
        case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
            code = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
            down = true;
            break;
        case WM_XBUTTONUP: code = GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2; break;
        case WM_MOUSEWHEEL: {
            const int notches = GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA;
            if (capturing) {
                if (notches != 0) controller_->keyCaptured(notches > 0 ? input::key::WheelUp : input::key::WheelDown);
            } else {
                controller_->keyboardMouse().wheel(notches, std::chrono::steady_clock::now());
            }
            return true;
        }
        case WM_INPUT: {
            if (!gameInput_) return false;
            RAWINPUT raw{};
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) &&
                raw.header.dwType == RIM_TYPEMOUSE && (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                controller_->keyboardMouse().mouseMoved(raw.data.mouse.lLastX, raw.data.mouse.lLastY);
            }
            return false; // Windows still has to tidy this message up
        }
        case WM_MOUSEMOVE: return gameInput_; // the pointer is of no interest while the game has the mouse
        default: return false;
    }

    if (capturing) {
        // The settings wait for a key. Esc ends the waiting without one; key-ups are of no interest.
        if (down) controller_->keyCaptured(code == VK_ESCAPE ? 0 : code);
        return true;
    }
    // Esc, F9 and F11 stay with the program: the menu, the display, full screen.
    if (input::isReservedKey(code)) return false;
    controller_->keyboardMouse().key(code, down);
    return !system;
}

void Application::toggleFullscreen(bool remember) {
    // Borderless window over the whole monitor: no mode switch, and Alt+Tab stays instant.
    const LONG_PTR style = GetWindowLongPtrW(window_, GWL_STYLE);
    if (!fullscreen_) {
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetWindowPlacement(window_, &windowedPlacement_) ||
            !GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            return;
        }
        // Set before the frame is worked out again: WM_NCCALCSIZE gives an enlarged window only the area
        // beside the taskbar, and a window enlarged before it went full screen still counts as enlarged.
        // Set after, the full screen kept a black strip the height of the taskbar (reported by the user).
        fullscreen_ = true;
        SetWindowLongPtrW(window_, GWL_STYLE, style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW | WS_MAXIMIZE));
        SetWindowPos(window_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        fullscreen_ = false;
        SetWindowLongPtrW(window_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(window_, &windowedPlacement_);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    taskbarPeek_ = false;
    edgeSince_ = {};
    awaySince_ = {};
    spdlog::info("Window is {}", fullscreen_ ? "full screen" : "a window");
    if (remember && controller_ && controller_->config().window.fullscreen != fullscreen_) {
        controller_->config().window.fullscreen = fullscreen_;
        controller_->configChanged();
    }
}

void Application::applyWindowAction(WindowAction action) {
    switch (action) {
        case WindowAction::Minimize: ShowWindow(window_, SW_MINIMIZE); break;
        case WindowAction::ToggleMaximize:
            if (!fullscreen_) ShowWindow(window_, IsZoomed(window_) ? SW_RESTORE : SW_MAXIMIZE);
            break;
        case WindowAction::ToggleFullscreen: toggleFullscreen(); break;
        // The same way the cross of a title bar takes: the window is asked to close and tidies up.
        case WindowAction::Close: PostMessageW(window_, WM_CLOSE, 0, 0); break;
    }
}

namespace {

/// The taskbar of Windows on the screen `monitor`: the main one, or one of those on further screens.
HWND taskbarOn(HMONITOR monitor) {
    if (const HWND main = FindWindowW(L"Shell_TrayWnd", nullptr); main && MonitorFromWindow(main, MONITOR_DEFAULTTONULL) == monitor) {
        return main;
    }
    for (HWND other = FindWindowExW(nullptr, nullptr, L"Shell_SecondaryTrayWnd", nullptr); other;
         other = FindWindowExW(nullptr, other, L"Shell_SecondaryTrayWnd", nullptr)) {
        if (MonitorFromWindow(other, MONITOR_DEFAULTTONULL) == monitor) return other;
    }
    return nullptr;
}

bool isTaskbar(HWND window) {
    wchar_t name[64] = {};
    if (!window || !GetClassNameW(GetAncestor(window, GA_ROOT), name, 64)) return false;
    return std::wstring_view(name) == L"Shell_TrayWnd" || std::wstring_view(name) == L"Shell_SecondaryTrayWnd";
}

} // namespace

void Application::updateTaskbarPeek(std::chrono::steady_clock::time_point now) {
    using std::chrono::milliseconds;
    // Only in full screen, and never while mouse and keys belong to the game.
    const HMONITOR screen = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor{sizeof(monitor)};
    if (!fullscreen_ || gameInput_ || !GetMonitorInfoW(screen, &monitor)) {
        edgeSince_ = {};
        taskbarPeek_ = false;
        return;
    }
    const RECT& all = monitor.rcMonitor;
    const RECT& work = monitor.rcWork;
    // Where the two differ is where the taskbar sits. If they do not differ,
    // this screen has none, or it hides by itself and Windows brings it out.
    const bool bottom = work.bottom < all.bottom;
    const bool top = work.top > all.top;
    const bool left = work.left > all.left;
    const bool right = work.right < all.right;
    if (!bottom && !top && !left && !right) return;
    POINT pointer{};
    if (!GetCursorPos(&pointer)) return;
    const bool onScreen = pointer.x >= all.left && pointer.x < all.right && pointer.y >= all.top && pointer.y < all.bottom;
    const bool atEdge = onScreen && ((bottom && pointer.y >= all.bottom - 1) || (top && pointer.y <= all.top) ||
                                     (left && pointer.x <= all.left) || (right && pointer.x >= all.right - 1));
    const bool overTaskbar = onScreen && ((bottom && pointer.y >= work.bottom) || (top && pointer.y < work.top) ||
                                          (left && pointer.x < work.left) || (right && pointer.x >= work.right));

    if (!taskbarPeek_) {
        if (!atEdge || GetForegroundWindow() != window_) {
            edgeSince_ = {};
            return;
        }
        if (edgeSince_ == std::chrono::steady_clock::time_point{}) edgeSince_ = now;
        // A moment of resting there, so that brushing the edge in passing does nothing.
        if (now - edgeSince_ < milliseconds(300)) return;
        const HWND taskbar = taskbarOn(screen);
        if (!taskbar) return;
        taskbarPeek_ = true;
        awaySince_ = {};
        // The window keeps its size, and so does the picture. Windows puts its taskbar in front of
        // a window that fills the screen once that window is no longer the active one; handing the
        // front to the taskbar brings it over the picture.
        SetForegroundWindow(taskbar);
        spdlog::info("Window: the taskbar comes over the picture{}",
                     isTaskbar(GetForegroundWindow()) ? "" : " (Windows did not hand it the front)");
        return;
    }
    if (overTaskbar) {
        awaySince_ = {};
        return;
    }
    if (awaySince_ == std::chrono::steady_clock::time_point{}) awaySince_ = now;
    if (now - awaySince_ < milliseconds(700)) return;
    taskbarPeek_ = false;
    edgeSince_ = {};
    // Left alone: the picture takes the front back, and the taskbar goes behind it again. Not when
    // something opened from the taskbar is in front now (a menu, another program); that stays.
    if (isTaskbar(GetForegroundWindow())) {
        SetForegroundWindow(window_);
        spdlog::info("Window: the taskbar went back behind the picture{}", GetForegroundWindow() == window_ ? "" : " (Windows kept it in front)");
    }
}

LRESULT Application::hitTest(HWND window, LPARAM lParam) const {
    POINT point{static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam))};
    RECT client{};
    if (!ScreenToClient(window, &point) || !GetClientRect(window, &client)) return HTCLIENT;
    // Filling the screen there is nothing to drag or to resize.
    if (fullscreen_) return HTCLIENT;
    const float barHeight = ui_ ? ui_->windowBarHeight() : 0.0f;
    const float buttonsWidth = ui_ ? ui_->windowBarButtonsWidth() : 0.0f;
    const bool overButtons = barHeight > 0.0f && static_cast<float>(point.y) < barHeight &&
                             static_cast<float>(point.x) >= static_cast<float>(client.right) - buttonsWidth;
    if (!IsZoomed(window)) {
        // The edges resize, as a frame from Windows would. Over the buttons only a hair's breadth, so they stay easy to hit.
        const int grip = std::max(4, static_cast<int>(6.0f * scale_));
        const int topGrip = overButtons ? std::max(1, static_cast<int>(2.0f * scale_)) : grip;
        const bool onLeft = point.x < grip;
        const bool onRight = point.x >= client.right - grip;
        const bool onTop = point.y < topGrip;
        const bool onBottom = point.y >= client.bottom - grip;
        if (onTop && onLeft) return HTTOPLEFT;
        if (onTop && onRight) return HTTOPRIGHT;
        if (onBottom && onLeft) return HTBOTTOMLEFT;
        if (onBottom && onRight) return HTBOTTOMRIGHT;
        if (onLeft) return HTLEFT;
        if (onRight) return HTRIGHT;
        if (onTop) return HTTOP;
        if (onBottom) return HTBOTTOM;
    }
    // The bar, where it is no button: the window is dragged by it, and a double click enlarges it.
    if (barHeight > 0.0f && static_cast<float>(point.y) < barHeight && !overButtons) return HTCAPTION;
    return HTCLIENT;
}

void Application::openHelp(std::string_view topic) {
    const bool german = ui::resolveLanguage(controller_->config().language, systemLanguage_) == ui::Language::German;
    const std::string url = helpUrl(topic, german);
    ShellExecuteW(window_, L"open", toWide(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

LRESULT CALLBACK Application::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self = reinterpret_cast<Application*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (self) return self->handleMessage(window, message, wParam, lParam);
    return DefWindowProcW(window, message, wParam, lParam);
}

LRESULT Application::handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    // Keys and mouse that belong to the game never reach the interface.
    if (controller_ && routeInput(message, wParam, lParam)) return 0;
    if (imguiReady_ && ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) return 1;

    switch (message) {
        case WM_NCCALCSIZE:
            // The whole window is ours to draw in: no title bar and no frame
            // from Windows. Returning 0 for the request with sizes says so.
            if (wParam == TRUE) {
                // Enlarged to the screen, Windows makes the window larger than
                // the screen by the frame it expects it to have. It has none,
                // so it gets exactly the area beside the taskbar.
                if (!fullscreen_ && IsZoomed(window)) {
                    auto* sizes = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam);
                    MONITORINFO monitor{sizeof(monitor)};
                    if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) sizes->rgrc[0] = monitor.rcWork;
                }
                return 0;
            }
            break;
        case WM_NCHITTEST: return hitTest(window, lParam);
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) device_.resize(LOWORD(lParam), HIWORD(lParam));
            // In the log, so a hang can be placed: before or after the window was put away?
            if ((wParam == SIZE_MINIMIZED) != minimised_) {
                minimised_ = wParam == SIZE_MINIMIZED;
                spdlog::info("Window {}", minimised_ ? "minimised" : "restored");
            }
            return 0;
        case WM_KILLFOCUS:
            // Whatever was held is let go: the key that comes up later goes to another window.
            if (gameInput_) setGameInput(false);
            break;
        case WM_ACTIVATE:
            spdlog::info("Window is {}", LOWORD(wParam) == WA_INACTIVE ? "no longer in front" : "in front");
            break;
        case WM_ENTERSIZEMOVE:
        case WM_ENTERMENULOOP:
            diag::setStep(kModalStep);
            break;
        case WM_DPICHANGED: {
            // Moved to a monitor with another scaling: adopt it and take the size Windows suggests.
            applyScale(scaleForDpi(HIWORD(wParam)));
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = static_cast<LONG>(kMinWidth * scale_);
            info->ptMinTrackSize.y = static_cast<LONG>(kMinHeight * scale_);
            return 0;
        }
        case WM_KEYDOWN:
            // F11 switches between window and full screen. Bit 30 is set for
            // repeats while the key is held.
            if (wParam == VK_F11 && (lParam & (1 << 30)) == 0) {
                toggleFullscreen();
                return 0;
            }
            break;
        case WM_SYSCOMMAND:
            // Alt alone would open the (non-existent) window menu and stall input.
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_DESTROY:
            window_ = nullptr;
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace veloplay
