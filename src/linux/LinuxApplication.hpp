#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "app/AppController.hpp"
#include "app/Installer.hpp"
#include "app/UiModel.hpp"
#include "linux/FfmpegRecorder.hpp"
#include "linux/GlCapture.hpp"
#include "linux/GlVideoRenderer.hpp"
#include "linux/SdlAudio.hpp"
#include "media/FramePacer.hpp"
#include "media/MediaPipeline.hpp"
#include "session/StreamMeter.hpp"
#include "ui/Theme.hpp"
#include "ui/UiManager.hpp"
#include "update/HttpClient.hpp"
#include "util/StallWatch.hpp"

struct SDL_Window;
union SDL_Event;
typedef struct SDL_GLContextState* SDL_GLContext;

namespace veloplay::linuxos {

/// The Linux shell around the client: an SDL window with OpenGL, the loop, and
/// the Dear ImGui glue. What it shows and does lives in AppController and
/// ui::UiManager, as on Windows; this class does there what
/// src/app/Application.cpp does with Win32 and Direct3D.
class LinuxApplication {
public:
    LinuxApplication(int argc, char** argv);
    ~LinuxApplication();

    LinuxApplication(const LinuxApplication&) = delete;
    LinuxApplication& operator=(const LinuxApplication&) = delete;

    /// Runs until the window is closed. Returns the process exit code.
    int run();

private:
    /// "--about", "--install", "--uninstall": done without a window. The exit code if it was such a command.
    std::optional<int> runCommand();
    void waitForPredecessor();
    bool createWindow();
    void configureController(const std::filesystem::path& dataDirectory);
    void frame();
    void handleEvent(const SDL_Event& event);
    /// True if the event was a key or mouse event that went to the game or to the settings.
    bool routeInput(const SDL_Event& event);
    void setGameInput(bool on);
    void toggleFullscreen(bool remember = true);
    void applyWindowAction(WindowAction action);
    void applyScale();
    void forgetPictures();
    void logStreamSummary(std::chrono::steady_clock::time_point now);
    void handleRecording(bool showVideo, media::ShownKind kind, std::chrono::steady_clock::time_point now);
    void stopRecording(const char* why);
    void recordingStopped(Result<record::RecordingSummary> outcome);
    void collectScreenshot();
    void startSuccessor(const std::filesystem::path& program);
    void openAddress(const std::string& address);
    [[nodiscard]] bool inFront() const;

    std::vector<std::string> arguments_;
    std::filesystem::path executable_;
    SDL_Window* window_ = nullptr;
    SDL_GLContext gl_ = nullptr;
    // The few OpenGL functions the loop calls itself, through SDL (nothing of OpenGL is linked).
    void (*bindFramebuffer_)(unsigned target, unsigned framebuffer) = nullptr;
    const unsigned char* (*getString_)(unsigned name) = nullptr;
    void (*viewport_)(int x, int y, int width, int height) = nullptr;
    void (*clearColor_)(float red, float green, float blue, float alpha) = nullptr;
    void (*clear_)(unsigned mask) = nullptr;
    input::IControllerBackend* controllerBackend_ = nullptr;
    bool imguiReady_ = false;
    bool quit_ = false;
    bool fullscreen_ = false;
    bool minimised_ = false;
    bool startedAfter_ = false;
    bool throughSony_ = false;
    bool restartRequested_ = false;
    bool successorStarted_ = false;
    std::filesystem::path successorProgram_;
    float scale_ = 1.0f;
    std::string systemLanguage_ = "en";
    std::string glVendor_;
    std::string glRenderer_;
    std::optional<WindowAction> pendingWindowAction_;

    std::unique_ptr<ISecretProtector> protector_;
    std::unique_ptr<update::IHttpClient> http_;
    std::unique_ptr<IInstaller> installer_; // outlives the controller, which copies with it
    GlVideoRenderer videoRenderer_;
    bool videoReady_ = false;
    GlFrameGrabber grabber_;
    struct Saved {
        bool ok = false;
        std::string where;
        std::string detail;
    };
    std::future<Saved> saving_;
    std::filesystem::path picturesFolder_;
    // Made before the sound output, which hands it every block of sound from its first one on.
    std::unique_ptr<VideoRecorder> recorder_;
    GlFrameStream recordStream_;
    std::future<Result<bool>> recordStarting_;
    std::future<Result<record::RecordingSummary>> recordStopping_;
    bool recordToggle_ = false;
    bool recordAvailable_ = false;
    std::string recordMissing_;
    std::filesystem::path videosFolder_;
    std::filesystem::path frameLogFolder_;
    std::filesystem::path recordFile_;
    int recordFps_ = 60;
    std::chrono::steady_clock::time_point lastRecorded_{};
    std::unique_ptr<SdlAudioOutput> audioOutput_;
    std::unique_ptr<SdlAudioOutput> hapticsOutput_;
    std::unique_ptr<SdlAudioInput> microphone_;
    std::unique_ptr<media::MediaPipeline> pipeline_;
    session::StreamMeter shownMeter_;
    std::deque<float> frameTimes_;
    std::chrono::steady_clock::time_point lastPicture_{};
    media::FramePacer pacer_;
    media::SoundReserve soundReserve_;
    media::InputRateDetector inputRate_;
    media::CadenceDetector cadence_;
    bool skipRepeats_ = false;
    bool loggedHalved_ = false;
    double displayHz_ = 0.0;
    std::chrono::steady_clock::time_point nextDisplayCheck_{};
    std::chrono::steady_clock::time_point nextRateSample_{};
    media::FrameChoice lastChoice_{};
    std::deque<media::VideoFrame> waiting_;
    std::uint64_t repeatedRefreshes_ = 0;
    std::uint64_t skippedPictures_ = 0;
    std::uint64_t overflowPictures_ = 0;
    std::uint64_t lastRealShown_ = 0;
    bool generatorFailureLogged_ = false;
    std::string loggedGenerator_;
    std::array<std::pair<std::uint64_t, std::chrono::steady_clock::time_point>, 8> arrivals_{};
    std::size_t arrivalsNext_ = 0;
    double insideMs_ = 0.0;
    bool gameInput_ = false;
    float mouseRestX_ = 0.0f; // the part of a mouse movement smaller than a count, kept for the next
    float mouseRestY_ = 0.0f;
    std::chrono::steady_clock::time_point lastRefresh_{};
    double refreshMs_ = 0.0;
    std::chrono::steady_clock::time_point nextSummary_{};
    bool screenSaverHeld_ = false;
    diag::StallWatch stallWatch_;
    // The rule that keeps the controller's touchpad away from the desktop (written through pkexec).
    std::future<bool> touchpadRule_;
    bool touchpadRuleFailed_ = false;

    std::mutex captureMutex_;
    std::vector<CaptureDevice> captureDevices_;
    std::jthread captureScan_;

    std::unique_ptr<AppController> controller_;
    std::unique_ptr<ui::UiManager> ui_;
    ui::Fonts fonts_;
    UiModel model_;
};

} // namespace veloplay::linuxos
