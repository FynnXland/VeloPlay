#pragma once

#include <memory>
#include <stop_token>
#include <thread>
#include <string>

#include <windows.h>

#include <array>
#include <deque>

#include "app/AppController.hpp"
#include "app/HangProbe.hpp"
#include "app/LoginWindow.hpp"
#include "audio/WasapiOutput.hpp"
#include "media/FramePacer.hpp"
#include "media/MediaPipeline.hpp"
#include "render/VideoRenderer.hpp"
#include "session/StreamMeter.hpp"
#include "util/StallWatch.hpp"
#include "util/ThreadStack.hpp"
#include "update/HttpClient.hpp"
#include "render/D3D11Device.hpp"
#include "ui/UiManager.hpp"

namespace veloplay {

/// The Windows shell around the client: window, message loop, Direct3D and the
/// Dear ImGui platform glue. Everything it shows and does lives in
/// AppController and ui::UiManager.
class Application {
public:
    Application();
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    /// Runs until the window is closed. Returns the process exit code.
    int run();

private:
    static LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT handleMessage(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    bool createWindow();
    void applyScale(float scale);
    void frame();
    void toggleFullscreen();
    void logStreamSummary(std::chrono::steady_clock::time_point now);
    void watchForHang(HWND window, std::stop_token stop);
    void openHelp(std::string_view topic);

    HWND window_ = nullptr;
    float scale_ = 1.0f;
    bool imguiReady_ = false;
    bool deviceLost_ = false;
    bool restartRequested_ = false;

    std::unique_ptr<ISecretProtector> protector_;
    std::unique_ptr<update::IHttpClient> http_;
    render::D3D11Device device_;
    // Declared before the controller, so they outlive it: the controller
    // tells the pipeline when a session ends.
    render::VideoRenderer videoRenderer_;
    bool videoReady_ = false;
    std::unique_ptr<audio::WasapiOutput> audioOutput_;
    std::unique_ptr<audio::WasapiOutput> hapticsOutput_;
    std::unique_ptr<media::MediaPipeline> pipeline_;
    session::StreamMeter shownMeter_; // pictures that reached the screen
    std::deque<float> frameTimes_;
    std::chrono::steady_clock::time_point lastPicture_{};
    media::FramePacer pacer_;             // which picture is due when
    media::InputRateDetector inputRate_;  // 30 or 60 fps, by what arrives
    std::chrono::steady_clock::time_point nextRateSample_{};
    media::FrameChoice lastChoice_{};     // what the last refresh showed
    bool generatorFailureLogged_ = false;
    bool rtxFailureLogged_ = false;
    // Which picture arrived when, for the newest few; and from that, how long pictures take through this PC.
    std::array<std::pair<std::uint64_t, std::chrono::steady_clock::time_point>, 8> arrivals_{};
    std::size_t arrivalsNext_ = 0;
    double insideMs_ = 0.0;
    bool timerRaised_ = false; // the system timer runs at 1 ms while a stream is shown
    std::chrono::steady_clock::time_point lastRefresh_{}; // when the picture for the last refresh was chosen
    double refreshMs_ = 0.0;                              // smoothed time between two refreshes
    std::chrono::steady_clock::time_point nextSummary_{};
    bool minimised_ = false;
    // The stack reader has to outlive the watch that uses it.
    diag::ThreadStack threadStack_;
    diag::StallWatch stallWatch_;
    diag::MessageTrace messageTrace_;
    std::jthread hangProbe_;
    LoginWindow loginWindow_;
    std::filesystem::path loginDataDirectory_;
    bool fullscreen_ = false;
    WINDOWPLACEMENT windowedPlacement_{sizeof(WINDOWPLACEMENT)};
    std::unique_ptr<AppController> controller_;
    std::unique_ptr<ui::UiManager> ui_;
    ui::Fonts fonts_;
    std::string systemLanguage_ = "en";
    UiModel model_;
};

} // namespace veloplay
