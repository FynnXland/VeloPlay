#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/UiModel.hpp"
#include "session/ISession.hpp"

struct IMFSourceReader;
struct IMFMediaSource;

namespace veloplay::capture {

/// The video sources Windows knows (capture cards, webcams). Takes a moment:
/// call it away from the window thread.
std::vector<CaptureDevice> listDevices(std::string* why = nullptr);

/// Reads pictures and sound from a Media Foundation source reader and hands
/// them on as they are: pictures as NV12, sound as 16-bit samples. A device
/// and a file give the same kind of reader, so the tests use a file.
class CaptureSource {
public:
    CaptureSource() = default;
    CaptureSource(const CaptureSource&) = delete;
    CaptureSource& operator=(const CaptureSource&) = delete;
    ~CaptureSource();

    /// A capture card or webcam by its id from listDevices(), and the sound input that belongs to it, if any.
    Result<void> openDevice(const std::string& id, const std::string& name);
    /// A video file instead of a device, for tests.
    Result<void> openFile(const std::filesystem::path& file);
    /// Starts handing on to `sink` from threads of its own.
    void start(session::IMediaSink* sink);
    /// Ends the reading and waits for its threads. That wait can be long, or without end: some devices (seen
    /// with Windows' virtual camera) do not return from a read when their source is shut down. Never call it
    /// on the window's thread.
    void stop();
    /// stop(), then gives back the device and everything opened for it. The destructor does the same; calling it
    /// first chooses the thread that happens on.
    void close();
    /// From now on nothing is handed on, whatever a read still returns. Returns at once.
    void detachSink() { sink_.store(nullptr); }

    [[nodiscard]] std::uint64_t pictures() const { return pictures_.load(); }
    [[nodiscard]] bool ended() const { return ended_.load(); }
    [[nodiscard]] std::string failure();
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    [[nodiscard]] bool sound() const { return soundReader_ != nullptr || fileHasSound_; }

private:
    void run(IMFSourceReader* reader, bool video, bool audio);
    Result<void> setUpVideo(IMFSourceReader* reader, bool pickBest);
    Result<void> setUpAudio(IMFSourceReader* reader);

    bool started_ = false; // MFStartup was called
    IMFSourceReader* videoReader_ = nullptr;
    IMFSourceReader* soundReader_ = nullptr;
    IMFMediaSource* videoSource_ = nullptr; // devices only; shut down to end a read that waits
    IMFMediaSource* soundSource_ = nullptr;
    bool fileHasSound_ = false;
    int width_ = 0;
    int height_ = 0;
    int stride_ = 0;
    media::ColorInfo color_{};
    int soundRate_ = 0;
    int soundChannels_ = 0;
    std::atomic<session::IMediaSink*> sink_{nullptr};
    std::thread videoThread_;
    std::thread soundThread_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> ended_{false};
    std::atomic<std::uint64_t> pictures_{0};
    std::mutex mutex_;
    std::string failure_;
};

/// A capture card behind the session interface, so the rest of the program
/// shows it like a stream from a console. There is nothing to send back:
/// input, the login passcode and rest mode do nothing.
class CaptureSession final : public session::ISession {
public:
    CaptureSession() = default;
    ~CaptureSession() override;

    Result<void> start(const session::SessionConfig& config, session::IMediaSink* sink) override;
    /// Returns at once: the reading is ended on a thread of its own (see CaptureSource::stop).
    void stop() override;
    /// Always: the program then lets go of the session on another thread instead of waiting for it.
    [[nodiscard]] bool slowToEnd() const override { return true; }
    void detachSink() override { source_.detachSink(); }
    std::vector<session::SessionEvent> pollEvents() override;
    void setControllerState(const input::ControllerState&, session::Clock::time_point) override {}
    Result<void> submitLoginPin(std::string_view) override { return Error{ErrorCode::InvalidArgument, "a capture card has no passcode"}; }
    Result<void> sleepConsole() override { return Error{ErrorCode::InvalidArgument, "a capture card cannot put a console to rest"}; }
    session::SessionStats stats(session::Clock::time_point now) override;

private:
    CaptureSource source_;
    std::thread opener_; // opening a device takes a moment; the window does not wait for it
    std::thread stopper_; // ending it can take longer still; the window does not wait for that either
    std::mutex mutex_;
    std::string openFailure_;
    bool opened_ = false;
    bool announced_ = false;
    bool quitSent_ = false;
    std::atomic<bool> stopped_{false};
    std::uint64_t countedPictures_ = 0;
    session::Clock::time_point countedAt_{};
    double fps_ = 0.0;
};

/// For AppController::Options::captureFactory.
session::SessionFactory captureSessionFactory();

} // namespace veloplay::capture
