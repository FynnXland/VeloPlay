#pragma once

#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <string>

#include <windows.h>

#include <array>
#include <deque>

#include "app/AppController.hpp"
#include "app/HangProbe.hpp"
#include "app/LoginWindow.hpp"
#include "audio/WasapiInput.hpp"
#include "audio/WasapiOutput.hpp"
#include "media/FramePacer.hpp"
#include "media/MediaPipeline.hpp"
#include "capture/CaptureSession.hpp"
#include "record/VideoRecorder.hpp"
#include "render/FrameGrabber.hpp"
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
    /// Switches between a window and the whole screen. With `remember` the choice is kept for the next start.
    void toggleFullscreen(bool remember = true);
    /// Carries out what the bar at the top of the window asked for in the frame before.
    void applyWindowAction(WindowAction action);
    /// In full screen the taskbar of Windows lies behind the window. It comes
    /// forward while the pointer rests on the screen's edge where it sits.
    void updateTaskbarPeek(std::chrono::steady_clock::time_point now);
    /// Which part of the window a point belongs to, for a window that draws its own frame.
    LRESULT hitTest(HWND window, LPARAM lParam) const;
    /// Gives keys and mouse to the game, or takes them back for the program.
    void setGameInput(bool on);
    /// True if the message was a key or mouse event that went to the game or to the settings.
    bool routeInput(UINT message, WPARAM wParam, LPARAM lParam);
    void logStreamSummary(std::chrono::steady_clock::time_point now);
    void watchForHang(HWND window, std::stop_token stop);
    void openHelp(std::string_view topic);
    /// The graphics device is gone (driver crash, driver update, card removed): from the next turn
    /// of the loop on, recoverDevice() makes everything on it anew instead of drawing.
    void markDeviceLost(const std::string& why);
    /// One attempt at a new device with all that lives on it. False: try again a moment later.
    bool recoverDevice(std::chrono::steady_clock::time_point now);
    /// Forgets the shown pictures and what was measured about them.
    void forgetPictures();

    HWND window_ = nullptr;
    float scale_ = 1.0f;
    bool imguiReady_ = false;
    bool dx11Ready_ = false; // ImGui draws with the current graphics device
    bool deviceLost_ = false;
    bool deviceGivenUp_ = false; // no new device came, or it got lost again and again: the program ends
    std::chrono::steady_clock::time_point lostSince_{};
    std::chrono::steady_clock::time_point nextRecovery_{};
    int recoveryAttempts_ = 0;
    unsigned vendorBeforeLoss_ = 0;
    std::deque<std::chrono::steady_clock::time_point> losses_; // of the last minute
    bool testDeviceLoss_ = false; // --test-device-loss: counts the device as lost a few seconds after the start
    bool throughSony_ = false;    // --through-sony: the way from away, tried at home
    std::chrono::steady_clock::time_point testLossAt_{};
    bool restartRequested_ = false;
    bool successorStarted_ = false; // the instance that takes over after an update is already running
    bool startedAfter_ = false;     // this instance was started by one before it and has to come to the front itself
    /// Starts `program` (the installed one, or this one again) so that it takes over from this instance.
    void startSuccessor(const std::filesystem::path& program);
    std::filesystem::path successorProgram_;
    void waitForPredecessor();
    /// "--install" and "--uninstall": done without a window. Gives the exit code if there was such a command.
    std::optional<int> runCommand();

    std::unique_ptr<ISecretProtector> protector_;
    std::unique_ptr<update::IHttpClient> http_;
    std::unique_ptr<IInstaller> installer_; // outlives the controller, which copies with it
    render::D3D11Device device_;
    // Declared before the controller, so they outlive it: the controller
    // tells the pipeline when a session ends.
    render::VideoRenderer videoRenderer_;
    // Screenshots: the copy of the picture, then packing and writing it beside the window.
    render::FrameGrabber grabber_;
    struct Saved {
        bool ok = false;
        std::string where;
        std::string detail;
    };
    std::future<Saved> saving_;
    std::filesystem::path picturesFolder_;
    void collectScreenshot();
    // Videos: copies of every picture go to the recorder, which writes them on its own thread.
    // Declared before the sound output, so it outlives the device thread that hands it the sound.
    std::unique_ptr<record::VideoRecorder> recorder_;
    render::FrameStream recordStream_;
    std::future<Result<bool>> recordStarting_;
    std::future<Result<record::RecordingSummary>> recordStopping_;
    bool recordToggle_ = false; // F10 was pressed; handled after the next picture is drawn
    bool recordAvailable_ = false;
    std::string recordMissing_;
    std::filesystem::path videosFolder_;
    std::filesystem::path frameLogFolder_;
    std::filesystem::path recordFile_;
    int recordFps_ = 60;
    std::chrono::steady_clock::time_point lastRecorded_{};
    void handleRecording(bool showVideo, media::ShownKind kind, std::chrono::steady_clock::time_point now);
    void stopRecording(const char* why);
    void recordingStopped(Result<record::RecordingSummary> outcome);
    bool videoReady_ = false;
    std::unique_ptr<audio::WasapiOutput> audioOutput_;
    std::unique_ptr<audio::WasapiOutput> hapticsOutput_;
    std::unique_ptr<audio::WasapiInput> microphone_; // for voice chat; records only while the setting is on
    std::unique_ptr<media::MediaPipeline> pipeline_;
    session::StreamMeter shownMeter_; // pictures that reached the screen
    std::deque<float> frameTimes_;
    std::chrono::steady_clock::time_point lastPicture_{};
    media::FramePacer pacer_;             // which picture is due when
    media::SoundReserve soundReserve_;    // sound kept against late packets, with the network buffer on "automatic"
    media::InputRateDetector inputRate_;  // 30 or 60 fps, by what arrives
    media::CadenceDetector cadence_;      // a game at 30 fps inside a 60 fps stream
    bool skipRepeats_ = false;            // its repeated pictures are measured but not shown
    bool loggedHalved_ = false;           // what the log said last about the rate of the game
    double displayHz_ = 0.0;              // refreshes per second of the screen the window is on
    std::chrono::steady_clock::time_point nextDisplayCheck_{};
    std::chrono::steady_clock::time_point nextRateSample_{};
    media::FrameChoice lastChoice_{};     // what the last refresh showed
    // Decoded pictures in front of the renderer, which holds two: each goes in when the pacer needs
    // it (FramePacer::wantsNext), so none pushes out a picture still shown and two that arrive within
    // one refresh go out on two.
    std::deque<media::VideoFrame> waiting_;
    // For the 10-second log line: refreshes without a new picture, real pictures never shown,
    // pictures dropped from a full queue.
    std::uint64_t repeatedRefreshes_ = 0;
    std::uint64_t skippedPictures_ = 0;
    std::uint64_t overflowPictures_ = 0;
    std::uint64_t lastRealShown_ = 0;
    bool generatorFailureLogged_ = false;
    std::string loggedGenerator_;         // the method for pictures in between that the log names last
    bool rtxFailureLogged_ = false;
    // Which picture arrived when, for the newest few; and from that, how long pictures take through this PC.
    std::array<std::pair<std::uint64_t, std::chrono::steady_clock::time_point>, 8> arrivals_{};
    std::size_t arrivalsNext_ = 0;
    double insideMs_ = 0.0;
    bool timerRaised_ = false; // the system timer runs at 1 ms while a stream is shown
    bool gameInput_ = false;   // keys and mouse belong to the game: pointer hidden and held, raw mouse movement
    std::chrono::steady_clock::time_point lastRefresh_{}; // when the picture for the last refresh was chosen
    double refreshMs_ = 0.0;                              // smoothed time between two refreshes
    std::chrono::steady_clock::time_point nextSummary_{};
    bool minimised_ = false;
    std::optional<WindowAction> pendingWindowAction_; // asked for while drawing, done before the next frame
    bool taskbarPeek_ = false;                         // the taskbar was handed the front and lies over the picture
    std::chrono::steady_clock::time_point edgeSince_{}; // since when the pointer rests on the taskbar's edge
    std::chrono::steady_clock::time_point awaySince_{}; // since when it has left the taskbar again
    // The stack reader has to outlive the watch that uses it.
    diag::ThreadStack threadStack_;
    diag::StallWatch stallWatch_;
    diag::MessageTrace messageTrace_;
    std::jthread hangProbe_;
    // Capture cards come and go with their cable; Windows is asked every few seconds, away from the window.
    // The thread comes after what it uses, so it is stopped and joined before those go.
    std::mutex captureMutex_;
    std::vector<CaptureDevice> captureDevices_;
    std::jthread captureScan_;
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
