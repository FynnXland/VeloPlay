#include "app/Application.hpp"

#include <dwmapi.h>
#include <objbase.h>
#include <shellapi.h>
#include <timeapi.h>

#include <imgui.h>
#include <imgui_impl_dx11.h>
#include <imgui_impl_win32.h>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/msvc_sink.h>
#include <spdlog/spdlog.h>

#include <utility>

#include "app/HangProbe.hpp"
#include "app/HelpLinks.hpp"
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
        ImGui_ImplDX11_Shutdown();
        ImGui_ImplWin32_Shutdown();
        ImGui::DestroyContext();
    }
    if (window_) DestroyWindow(window_);
    UnregisterClassW(kWindowClass, GetModuleHandleW(nullptr));
}

int Application::run() {
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

    protector_ = createDpapiProtector();
    AppController::Options options;
    options.configPath = dataDirectory / "config.json";
    options.protector = protector_.get();
    options.controllerBackend = input::createSdlControllerBackend();
    options.openHelp = [this](std::string_view topic) { openHelp(topic); };
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
    audioOutput_ = std::make_unique<audio::WasapiOutput>();
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
        PostMessageW(window_, WM_CLOSE, 0, 0);
    };
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

    // The second watch asks Windows itself. A window can count as "not
    // responding" while its loop runs (issue 3); the first watch cannot see that.
    hangProbe_ = std::jthread([this, window = window_](std::stop_token stop) { watchForHang(window, stop); });

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
        if (deviceLost_) break;
        frame();
    }

    if (std::exchange(timerRaised_, false)) timeEndPeriod(1);
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
        // Shut everything down first so the new instance finds ports and files free.
        ui_.reset();
        controller_.reset();
        const std::filesystem::path exe = executablePath();
        ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, exe.parent_path().c_str(), SW_SHOWNORMAL);
        return 0;
    }

    if (deviceLost_) {
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

    // Dark title bar to match the content (Windows 10 2004 and later).
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
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

    controller_->update();
    diag::setStep("collecting what to show");
    controller_->fillModel(model_);
    model_.gpuName = device_.adapterName();
    model_.reducedMotion = reducedMotion();
    model_.rtxVideoAvailable = videoRenderer_.rtxAvailable();

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
            videoRenderer_.clear();
            frameTimes_.clear();
            lastPicture_ = {};
            lastChoice_ = {};
            pacer_.reset();
            lastRefresh_ = {};
            refreshMs_ = 0.0;
            insideMs_ = 0.0;
            arrivals_ = {};
            inputRate_.reset(config.stream.fps);
            nextRateSample_ = {};
        } else if (model_.stream && now >= nextRateSample_) {
            // Once a second: is it a 30 or a 60 fps stream that arrives?
            nextRateSample_ = now + std::chrono::seconds(1);
            inputRate_.addSample(model_.stream->streamFps);
        }
        // What arrives decides what is made of it: the factor for frame
        // generation follows the measured rate when it is set to automatic.
        const int wanted = media::frameGenerationFactorFor(config.graphics.frameGenerationFactor, inputRate_.fps());
        const bool available = videoRenderer_.frameGenerationAvailable();
        pacer_.configure(available ? wanted : 1, std::chrono::microseconds(1'000'000 / inputRate_.fps()));

        // A console on Wi-Fi delivers in bursts with gaps of tens of milliseconds
        // between them; chiaki-ng keeps about 100 ms of sound for that reason.
        // Pictures held back for frame generation hold the sound back as well.
        const std::chrono::milliseconds reserve{config.network.jitter == JitterMode::Stable ? 140 : 80};
        pipeline_->setAudioDelay(reserve + std::chrono::duration_cast<std::chrono::milliseconds>(pacer_.addedDelay()));

        if (model_.stream && videoRenderer_.hasPicture()) {
            const media::MediaPipeline::Stats media = pipeline_->stats();
            StreamStats& stats = *model_.stream;
            stats.presenting = true;
            stats.outputFps = static_cast<float>(shownMeter_.rates(now).framesPerSecond);
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

    // Waits for the display here, where no lock is held, instead of in present().
    diag::setStep("waiting for the display");
    device_.waitForFrame(100);

    // The picture is fetched after the wait, not before it: whatever arrived
    // while waiting goes out with this refresh instead of the next one.
    render::VideoRenderer::Output output;
    if (pipeline_ && streaming) {
        diag::setStep("taking over a picture");
        const auto now = std::chrono::steady_clock::now();
        for (const media::VideoFrame& picture : pipeline_->takeFrames()) {
            if (!videoRenderer_.submit(picture)) continue;
            // Laid out by when the decoder finished, not by when this loop came around.
            const bool stamped = picture.decodedAt != std::chrono::steady_clock::time_point{};
            pacer_.onFrame(videoRenderer_.newestId(), stamped ? picture.decodedAt : now);
            arrivals_[arrivalsNext_++ % arrivals_.size()] = {videoRenderer_.newestId(), picture.receivedAt};
            if (!videoRenderer_.frameGenerationAvailable() && !generatorFailureLogged_) {
                generatorFailureLogged_ = true;
                spdlog::warn("Frame generation is not available: {}", videoRenderer_.frameGeneratorError());
            }
        }
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
        output.choice = pacer_.choose(now, slack);
        output.upscale = config.graphics.superResolutionLevel > 0;
        output.rtx = config.graphics.superResolutionLevel >= 2;
        if (output.rtx && !videoRenderer_.rtxAvailable() && !rtxFailureLogged_ && !videoRenderer_.rtxError().empty()) {
            rtxFailureLogged_ = true;
            spdlog::warn("RTX Video Super Resolution is not used: {}", videoRenderer_.rtxError());
        }
        output.sharpness = config.graphics.sharpness;
        // A picture counts as shown when it differs from the one before, real or computed.
        if (output.choice.valid && !(output.choice == lastChoice_)) {
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
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    diag::setStep("presenting");
    if (!device_.present(streaming ? config.graphics.vsync : true)) deviceLost_ = true;
    diag::setStep("idle");

    // The dashboard does not need 60 redraws per second while another window
    // has focus. A running stream does.
    if (!streaming && GetForegroundWindow() != window_) Sleep(40);
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
                 "(slowest {:.1f} ms), {} decoded, {} dropped, {} failed, frame generation {}x, {:.0f} ms inside the PC{}",
                 stream.streamFps, stream.outputFps, stream.bitrateMbps,
                 stream.packetLossPercent, media.decodeMs, media.decodeMaxMs, media.decodedFrames, media.droppedFrames,
                 media.failedFrames, pacer_.factor(), insideMs_, IsIconic(window_) ? ", window minimised" : "");
}

void Application::toggleFullscreen() {
    // Borderless window over the whole monitor: no mode switch, and Alt+Tab stays instant.
    const LONG_PTR style = GetWindowLongPtrW(window_, GWL_STYLE);
    if (!fullscreen_) {
        MONITORINFO monitor{sizeof(monitor)};
        if (!GetWindowPlacement(window_, &windowedPlacement_) ||
            !GetMonitorInfoW(MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST), &monitor)) {
            return;
        }
        SetWindowLongPtrW(window_, GWL_STYLE, style & ~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
        SetWindowPos(window_, HWND_TOP, monitor.rcMonitor.left, monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left, monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrW(window_, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(window_, &windowedPlacement_);
        SetWindowPos(window_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    fullscreen_ = !fullscreen_;
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
    if (imguiReady_ && ImGui_ImplWin32_WndProcHandler(window, message, wParam, lParam)) return 1;

    switch (message) {
        case WM_SIZE:
            if (wParam != SIZE_MINIMIZED) device_.resize(LOWORD(lParam), HIWORD(lParam));
            // In the log, so a hang can be placed: before or after the window was put away?
            if ((wParam == SIZE_MINIMIZED) != minimised_) {
                minimised_ = wParam == SIZE_MINIMIZED;
                spdlog::info("Window {}", minimised_ ? "minimised" : "restored");
            }
            return 0;
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
