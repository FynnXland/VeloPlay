// Renders the client's screens offscreen and writes them as BMP files.
//
//   ui_preview <output directory> [--scale 1.5] [--lang de|en]
//
// On a machine without a display, run it under a virtual one:
//   xvfb-run -a ./ui_preview out

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <imgui_internal.h>

#include "ui/UiManager.hpp"
#include "ui/Widgets.hpp"

using namespace veloplay;

namespace {

/// The preview only looks; nothing the user could do has an effect.
class NoActions final : public IUiActions {
public:
    void wake(std::string_view) override {}
    void connect(std::string_view) override {}
    void disconnect() override {}
    void sleepAndDisconnect() override {}
    void applyStreamSettings() override {}
    void submitLoginPin(std::string_view) override {}
    void forgetConsole(std::string_view) override {}
    void startPairing(std::string_view, std::string_view, std::string_view) override {}
    void cancelPairing() override {}
    void dismissPairing() override {}
    void dismissNotice(std::size_t) override {}
    void selectProfile(std::string_view) override {}
    void captureKey() override {}
    void windowAction(WindowAction) override {}
    void testVibration() override {}
    void testTriggers(int) override {}
    void endKeyCapture() override {}
    void configChanged() override {}
    void openHelp(std::string_view) override {}
    void startConnectionTest(std::string_view) override {}
    void stopConnectionTest() override {}
    void applyStreamAdvice() override {}
    void checkForUpdates() override {}
    void installUpdate() override {}
    void restart() override {}
    void installProgram() override {}
    void takeScreenshot() override {}
    void openPicturesFolder() override {}
    void toggleRecording() override {}
    void showCapture(const std::string&) override {}
    void captureWheel(int) override {}
    void endWheelCapture() override {}
    void chooseWheel(const std::string&) override {}
    void openVideosFolder() override {}
    void startAccountLogin() override {}
    void startAccountLoginInBrowser() override {}
    void submitAccountRedirect(std::string_view) override {}
    void dismissAccountLookup() override {}
};

/// Response times in ms for the connection scenes; negative means lost.
protocol::LatencySnapshot sampleLink(bool steady) {
    protocol::LatencyStats stats(120);
    for (int i = 0; i < 60; ++i) {
        if (steady) {
            stats.add(3.2f + 0.6f * static_cast<float>((i * 7) % 5) / 4.0f);
        } else if (i % 11 == 4) {
            stats.add(std::nullopt);
        } else {
            stats.add(9.0f + static_cast<float>((i * 37) % 23) * 2.1f + (i % 13 == 0 ? 55.0f : 0.0f));
        }
    }
    return stats.snapshot();
}

struct Scene {
    std::string name;
    std::function<void(UiModel&, ui::UiManager&)> setup;
    /// Optional: what the controller does in each frame, for pictures of steering with it.
    std::function<void(UiModel&, int frame)> step = {};
};

ConsoleEntry console(std::string name, std::string hostId, std::string address, ConsoleStatus status, bool registered) {
    ConsoleEntry entry;
    entry.name = std::move(name);
    entry.hostId = std::move(hostId);
    entry.address = std::move(address);
    entry.status = status;
    entry.registered = registered;
    entry.credentialsValid = registered;
    return entry;
}

/// A stream as it looks when everything works: 30 fps in, 60 fps on screen.
StreamStats playingStats() {
    StreamStats stats;
    stats.presenting = true;
    stats.streamFps = 30.0f;
    stats.outputFps = 60.0f;
    stats.inputWidth = 1920;
    stats.inputHeight = 1080;
    stats.outputWidth = 2560;
    stats.outputHeight = 1440;
    stats.scaler = "RTX Video";
    stats.insidePcMs = 24.0f;
    stats.frameGenerationFactor = 2;
    stats.frameGenerationActive = true;
    stats.frameGenerationEngine = "VeloPlay Flow";
    stats.addedLatencyMs = 16.7f;
    stats.bitrateMbps = 12.1f;
    stats.roundTripMs = 4.2f;
    stats.packetLossPercent = 0.1f;
    stats.decodeMs = 0.6f;
    stats.decodeMaxMs = 1.4f;
    stats.decoder = "HEVC, D3D11VA";
    for (int i = 0; i < 90; ++i) stats.frameTimes.push_back(16.7f + (i % 31 == 9 ? 9.0f : 0.3f * static_cast<float>(i % 3)));
    return stats;
}

void withController(UiModel& model) {
    model.controllerConnected = true;
    model.controller.name = "DualSense Wireless Controller";
    model.controller.type = input::ControllerType::DualSense;
    model.controller.connection = input::ConnectionType::Usb;
    model.controllers = {model.controller};
}

void withConsoles(UiModel& model) {
    withController(model);
    ConsoleEntry living = console("Wohnzimmer", "001122AABBCC", "192.168.1.20", ConsoleStatus::Ready, true);
    living.runningApp = "Astro Bot";
    model.consoles = {living, console("Arbeitszimmer", "001122AABBDD", "192.168.1.31", ConsoleStatus::Standby, true),
                      console("PS5-417", "001122AABBEE", "192.168.1.44", ConsoleStatus::Ready, false)};
}

/// Keys and mouse switched on, with a layout of its own for the game that runs.
void withGameLayout(UiModel& model) {
    input::KeyboardMouseSettings& kbm = model.config->controller.keyboardMouse;
    kbm.enabled = true;
    input::GameKeyLayout game;
    game.titleId = "PPSA21567";
    game.name = "Astro Bot";
    game.keys = kbm.keys;
    game.keys[static_cast<std::size_t>(input::PadControl::Cross)] = input::key::Space;
    game.keys[static_cast<std::size_t>(input::PadControl::Square)] = 'E';
    kbm.games = {game};
    model.currentGameId = game.titleId;
    model.currentGameName = game.name;
}

std::vector<Scene> scenes() {
    using ui::UiManager;
    return {
        {"01-searching", [](UiModel& model, UiManager&) { model.discoveryRunning = true; }},
        {"02-dashboard", [](UiModel& model, UiManager&) { withConsoles(model); }},
        {"35-dashboard-with-controller", [](UiModel& model, UiManager&) { withConsoles(model); },
         [](UiModel& model, int frame) {
             // Down twice on the directional pad, each a short press.
             const bool down = (frame >= 20 && frame < 23) || (frame >= 40 && frame < 43);
             model.controllerState.buttons = down ? static_cast<std::uint32_t>(input::Button::DpadDown) : 0u;
         }},
        {"36-settings-with-controller",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             ui.openSettings(UiManager::SettingsTab::Picture);
         },
         [](UiModel& model, int frame) {
             const bool down = (frame >= 20 && frame < 23) || (frame >= 40 && frame < 43) || (frame >= 60 && frame < 63);
             model.controllerState.buttons = down ? static_cast<std::uint32_t>(input::Button::DpadDown) : 0u;
         }},
        {"03-waking",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.state = AppState::Waking;
             model.consoles[1].waking = true;
             model.config->activeProfile = std::string(kProfileNative60);
         }},
        {"04-notices",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.consoles.resize(2);
             model.consoles[0].status = ConsoleStatus::Offline;
             model.consoles[0].runningApp.clear();
             model.consoles[1].credentialsValid = false;
             model.notices = {{Notice::Kind::Error, Notice::Topic::WakeTimedOut, "Wohnzimmer"},
                              {Notice::Kind::Error, Notice::Topic::RemoteApiFailed, "9876"}};
         }},
        {"05-pair-console",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             ui.openPairing();
             ui.setPairingInputs("192.168.1.44", "", "");
         }},
        {"06-pair-account",
         [](UiModel& model, UiManager& ui) {
             model.accountLoginAvailable = true;
             withConsoles(model);
             ui.openPairing("192.168.1.44");
             ui.setPairingInputs("192.168.1.44", "fynn_online_id", "");
         }},
        {"07-pair-pin",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             ui.openPairing("192.168.1.44");
             ui.setPairingInputs("192.168.1.44", "ASNFZ4mrze8=", "1234 5678");
             ui.setPairingStep(UiManager::PairingStep::Pin);
         }},
        {"08-pair-running",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             ui.openPairing("192.168.1.44");
             model.pairing.phase = PairingStatus::Phase::Running;
         }},
        {"09-pair-failed",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             ui.openPairing("192.168.1.44");
             model.pairing = {PairingStatus::Phase::Failed, {}, ErrorCode::Rejected,
                              "the console rejected the pairing (HTTP 403): registration failed, most likely a wrong PIN"};
         }},
        {"10-pair-done",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             ui.openPairing("192.168.1.44");
             model.pairing.phase = PairingStatus::Phase::Succeeded;
             model.pairing.consoleName = "PS5-417";
             model.pairing.hostId = "AABBCCDDEEFF";
         }},
        {"11-settings-stream",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             ui.openSettings(UiManager::SettingsTab::Stream);
         }},
        {"12-settings-picture",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.rtxVideoAvailable = true;
             model.gpuVendor = UiModel::GpuVendor::Nvidia;
             model.config->graphics.superResolutionLevel = 2;
             ui.openSettings(UiManager::SettingsTab::Picture);
         }},
        {"48-dashboard-capture",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.captureDevices = {{"usb#1", "USB3 Capture"}, {"usb#2", "Integrated Camera"}};
         }},
        {"49-pause-menu-capture",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.state = AppState::Streaming;
             model.session.consoleName = "USB3 Capture";
             model.session.capture = true;
             model.session.connectedSeconds = 75;
             model.stream = playingStats();
             ui.showPauseMenu();
         }},
        {"50-settings-wheel",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.config->controller.wheel.device = "Logitech G29 Driving Force Racing Wheel";
             model.config->controller.wheel.enabled = true;
             model.config->controller.wheel.steerAxis = 0;
             model.config->controller.wheel.gas = {2, 32767, -32768};
             model.config->controller.wheel.buttons[0] = 0;
             model.config->controller.wheel.buttons[1] = 2;
             model.wheel.devices = {{3, "Logitech G29 Driving Force Racing Wheel", true}};
             model.wheel.present = true;
             model.wheel.force = true;
             model.wheel.steer = -0.4f;
             model.wheel.gas = 0.7f;
             model.wheel.capturing = static_cast<int>(input::WheelTarget::Brake);
             model.wheel.pressed = input::mask(input::Button::Cross);
             ui.openSettings(UiManager::SettingsTab::Wheel);
         }},
        {"47-settings-picture-amd",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.gpuName = "AMD Radeon RX 7800 XT";
             model.gpuVendor = UiModel::GpuVendor::Amd;
             model.rtxVideoAvailable = false;
             model.config->graphics.amdFrc = true;
             model.amdFrcActive = true;
             ui.openSettings(UiManager::SettingsTab::Picture);
         }},
        {"13-settings-controller",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             // A controller in use: Cross held, the left stick pushed up and right, R2 half way down.
             model.controller.reportsPerSecond = 250.0f;
             model.controller.batteryPercent = 80;
             model.controller.charging = true;
             model.controllerState.buttons = static_cast<std::uint32_t>(input::Button::Cross);
             model.controllerState.leftX = 20000;
             model.controllerState.leftY = -26000;
             model.controllerState.r2 = 128;
             ui.openSettings(UiManager::SettingsTab::Controller);
         }},
        {"37-console-menu",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             ui.showConsoleMenu(model.consoles.front().hostId, false);
         }},
        {"38-console-menu-question",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             ui.showConsoleMenu(model.consoles.front().hostId, true);
         }},
        {"30-settings-mouse-keyboard",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             withGameLayout(model);
             ui.openSettings(UiManager::SettingsTab::Keyboard);
         }},
        {"33-stream-key-layout",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             withGameLayout(model);
             model.state = AppState::Streaming;
             model.session.consoleName = "Wohnzimmer";
             model.session.connectedSeconds = 240;
             model.stream = playingStats();
             model.config->network.statsOverlay = false;
             ui.showKeyOverlay();
         }},
        {"31-settings-key-layout",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.config->controller.keyboardMouse.enabled = true;
             ui.openSettings(UiManager::SettingsTab::Keyboard);
             ui.scrollSettingsTo(0.5f);
         }},
        {"39-settings-install",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.updaterAvailable = true;
             model.install.available = true;
             model.install.target = "C:\\Users\\Jörg\\AppData\\Local\\Programs\\VeloPlay";
             ui.openSettings(UiManager::SettingsTab::Advanced);
             ui.scrollSettingsTo(0.35f);
         }},
        {"40-settings-install-failed",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.updaterAvailable = true;
             model.install.available = true;
             model.install.target = "C:\\Users\\Jörg\\AppData\\Local\\Programs\\VeloPlay";
             model.install.error = "could not copy avcodec-63.dll: There is not enough space on the disk.";
             ui.openSettings(UiManager::SettingsTab::Advanced);
             ui.scrollSettingsTo(0.35f);
         }},
        {"41-settings-installed",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.updaterAvailable = true;
             model.install.available = true;
             model.install.installed = true;
             ui.openSettings(UiManager::SettingsTab::Advanced);
             ui.scrollSettingsTo(0.35f);
         }},
        {"44-settings-capture",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.picturesFolder = "C:\\Users\\Jörg\\Pictures\\VeloPlay";
             model.recording.available = true;
             model.videosFolder = "C:\\Users\\Jörg\\Videos\\VeloPlay";
             ui.openSettings(UiManager::SettingsTab::Capture);
         }},
        {"46-settings-capture-missing",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.picturesFolder = "C:\\Users\\Jörg\\Pictures\\VeloPlay";
             model.recording.missing = "this PC has no Media Foundation (Windows N editions need the Media Feature Pack)";
             ui.openSettings(UiManager::SettingsTab::Capture);
         }},
        {"45-stream-recording",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.state = AppState::Streaming;
             model.session.consoleName = "Wohnzimmer";
             model.session.connectedSeconds = 240;
             model.stream = playingStats();
             model.microphoneAvailable = true;
             model.microphoneLive = true;
             model.config->audio.microphone = true;
             model.recording.available = true;
             model.recording.running = true;
             model.recording.seconds = 83.0;
         }},
        {"14-settings-advanced",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.config->remoteApi.enabled = true;
             model.config->manualHosts = {"10.0.20.5"};
             model.remoteApiRunning = true;
             model.discordAvailable = true;
             model.config->discord.gameName = "Astro's Playroom";
             ui.openSettings(UiManager::SettingsTab::Advanced);
         }},
        {"15-update-available",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.updaterAvailable = true;
             model.update.phase = update::UpdateStatus::Phase::Available;
             model.update.current = {0, 5, 0};
             model.update.latest = {0, 6, 0};
         }},
        {"16-update-downloading",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.updaterAvailable = true;
             model.update.phase = update::UpdateStatus::Phase::Downloading;
             model.update.latest = {0, 6, 0};
             model.update.progress = 0.62f;
         }},
        {"17-update-restart",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.updaterAvailable = true;
             model.update.phase = update::UpdateStatus::Phase::RestartNeeded;
             model.update.latest = {0, 6, 0};
         }},
        {"18-connection-good",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             ui.openConnection("001122AABBCC");
             model.connectionTest = {true, "001122AABBCC", "Wohnzimmer", sampleLink(true), {}};
             model.connectionTest.quality = protocol::rateLink(model.connectionTest.stats);
         }},
        {"19-connection-poor",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             ui.openConnection("001122AABBDD");
             model.connectionTest = {true, "001122AABBDD", "Arbeitszimmer", sampleLink(false), {}};
             model.connectionTest.quality = protocol::rateLink(model.connectionTest.stats);
         }},
        {"20-stream-connected",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.state = AppState::Streaming;
             model.session.consoleName = "Wohnzimmer";
             model.session.videoFrames = 5412;
             model.session.audioPackets = 18040;
             model.session.receivedMegabytes = 271.4;
             StreamStats stats;
             stats.streamFps = 30.0f;
             stats.bitrateMbps = 12.1f;
             stats.roundTripMs = 4.2f;
             stats.packetLossPercent = 0.1f;
             model.stream = stats;
         }},
        {"21-settings-updates",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.updaterAvailable = true;
             model.update.phase = update::UpdateStatus::Phase::UpToDate;
             model.update.current = {0, 5, 0};
             ui.openSettings(UiManager::SettingsTab::Advanced);
         }},
        {"29-stream-notes",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.state = AppState::Streaming;
             model.microphoneAvailable = true;
             model.microphoneLive = true;
             model.session.consoleName = "Wohnzimmer";
             model.session.connectedSeconds = 240;
             model.stream = playingStats();
             model.alerts = {StreamAlert::NoController, StreamAlert::ConnectionStalls};
         }},
        {"53-settings-notes",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.config->notes.hidden = {"battery_low", "fewer_pictures"};
             ui.openSettings(UiManager::SettingsTab::Advanced);
             ui.showNoteChoices();
         }},
        {"54-stream-notes-switched-off",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.state = AppState::Streaming;
             model.session.consoleName = "Wohnzimmer";
             model.session.connectedSeconds = 240;
             model.stream = playingStats();
             // The battery note is switched off: only the Wi-Fi one shows.
             model.config->notes.hidden = {"battery_low"};
             model.alerts = {StreamAlert::BatteryLow, StreamAlert::UnsteadyLink};
         }},
        {"52-stream-window-bar",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.state = AppState::Streaming;
             model.windowFullscreen = true;
             model.session.consoleName = "Wohnzimmer";
             model.session.connectedSeconds = 240;
             model.stream = playingStats();
         },
         // The pointer goes to the top edge: the bar comes over the picture.
         [](UiModel&, int frame) { ImGui::GetIO().AddMousePosEvent(600.0f, frame < 10 ? 300.0f : 1.0f); }},
        {"51-stream-graphics-restored",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.state = AppState::Streaming;
             model.session.consoleName = "Wohnzimmer";
             model.session.connectedSeconds = 240;
             model.stream = playingStats();
             model.alerts = {StreamAlert::PicturesLost, StreamAlert::GraphicsRestored, StreamAlert::UnsteadyLink};
         }},
        {"42-stream-screenshot",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.state = AppState::Streaming;
             model.session.consoleName = "Wohnzimmer";
             model.session.connectedSeconds = 240;
             model.stream = playingStats();
             model.captureNote.serial = 1;
             model.captureNote.saved = true;
             model.captureNote.where = "C:\\Users\\Jörg\\Pictures\\VeloPlay";
         }},
        {"43-stream-display-top-right",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.state = AppState::Streaming;
             model.session.consoleName = "Wohnzimmer";
             model.session.connectedSeconds = 240;
             model.stream = playingStats();
             model.config->network.statsCorner = 1;
             model.microphoneAvailable = true;
             model.microphoneLive = true;
             model.config->audio.microphone = true;
         }},
        {"32-stream-upscaling-note",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.state = AppState::Streaming;
             model.session.consoleName = "Wohnzimmer";
             model.session.connectedSeconds = 240;
             model.stream = playingStats();
             model.rtxVideoAvailable = true;
             model.config->graphics.superResolutionLevel = 2;
             ui.showUpscalingNote();
         }},
        {"34-stream-small-display",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.state = AppState::Streaming;
             model.session.consoleName = "Wohnzimmer";
             model.session.connectedSeconds = 240;
             model.stream = playingStats();
             model.config->network.statsCompact = true;
         }},
        {"26-stream-playing",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.state = AppState::Streaming;
             model.session.consoleName = "Wohnzimmer";
             model.session.connectedSeconds = 3;
             model.stream = playingStats();
         }},
        {"27-pause-menu",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.state = AppState::Streaming;
             model.session.consoleName = "Wohnzimmer";
             model.session.connectedSeconds = 240;
             model.session.videoFrames = 7200;
             model.session.audioPackets = 24000;
             model.session.receivedMegabytes = 362.0;
             model.session.hostId = "001122AABBCC";
             model.controllerConnected = true;
             model.controller.batteryPercent = 80;
             model.rtxVideoAvailable = true;
             model.config->graphics.superResolutionLevel = 2;
             model.microphoneAvailable = true;
             model.microphoneLive = true;
             model.config->audio.microphone = true;
             model.stream = playingStats();
             ui.showPauseMenu();
         }},
        {"55-pause-menu-away",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.state = AppState::Streaming;
             model.session.consoleName = "Wohnzimmer";
             model.session.connectedSeconds = 240;
             model.session.hostId = "001122AABBCC";
             model.session.away = true; // through the VPN from a friend's place
             model.controllerConnected = true;
             model.stream = playingStats();
             ui.showPauseMenu();
         }},
        {"56-stream-away-note",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.state = AppState::Streaming;
             model.session.consoleName = "Wohnzimmer";
             model.session.connectedSeconds = 240;
             model.session.away = true;
             model.stream = playingStats();
             model.alerts = {StreamAlert::UnsteadyLink};
         }},
        {"28-settings-in-game",
         [](UiModel& model, UiManager& ui) {
             withConsoles(model);
             model.state = AppState::Streaming;
             model.session.consoleName = "Wohnzimmer";
             model.session.connectedSeconds = 240;
             model.session.settingsChanged = true;
             model.controllerConnected = true;
             // Values chosen by hand, so the controls behind "automatic" show as well.
             model.config->activeProfile = std::string(kProfileCustom);
             model.config->stream.fps = 60;
             model.stream = playingStats();
             ui.showSettingsInGame(UiManager::SettingsTab::Stream);
         }},
        {"22-connecting",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.state = AppState::Connecting;
             model.session.consoleName = "Wohnzimmer";
         }},
        {"23-reconnecting",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.state = AppState::Reconnecting;
             model.session.consoleName = "Wohnzimmer";
             model.session.attempt = 2;
             model.session.attempts = 3;
         }},
        {"24-login-passcode",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.state = AppState::Connecting;
             model.session.consoleName = "Wohnzimmer";
             model.session.loginPinRequested = true;
             model.session.loginPinWrong = true;
         }},
        {"25-connection-failed",
         [](UiModel& model, UiManager&) {
             withConsoles(model);
             model.state = AppState::Error;
             model.notices = {{Notice::Kind::Error, Notice::Topic::SessionInUse, "Wohnzimmer",
                               "Remote is already in use"}};
         }},
    };
}

bool writeBmp(const std::filesystem::path& path, int width, int height) {
    // glReadPixels and BMP agree on bottom-up rows padded to four bytes.
    const int rowSize = (width * 3 + 3) & ~3;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(rowSize) * static_cast<std::size_t>(height));
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glReadPixels(0, 0, width, height, GL_BGR, GL_UNSIGNED_BYTE, pixels.data());

    unsigned char header[54] = {'B', 'M'};
    const auto put32 = [&](int offset, std::uint32_t value) { std::memcpy(header + offset, &value, 4); };
    put32(2, static_cast<std::uint32_t>(54 + pixels.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, static_cast<std::uint32_t>(width));
    put32(22, static_cast<std::uint32_t>(height));
    header[26] = 1;
    header[28] = 24;
    put32(34, static_cast<std::uint32_t>(pixels.size()));

    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(header), sizeof(header));
    file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
    return static_cast<bool>(file);
}

const char* firstExisting(std::initializer_list<const char*> candidates) {
    for (const char* path : candidates) {
        if (std::filesystem::exists(path)) return path;
    }
    return "";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: ui_preview <output directory> [--scale 1.5] [--lang de|en] [--size 820x560]\n");
        return 2;
    }
    const std::filesystem::path output = argv[1];
    float scale = 1.0f;
    std::string language = "de";
    int baseWidth = 1100;
    int baseHeight = 720;
    for (int i = 2; i + 1 < argc; i += 2) {
        if (std::strcmp(argv[i], "--scale") == 0) scale = std::stof(argv[i + 1]);
        if (std::strcmp(argv[i], "--lang") == 0) language = argv[i + 1];
        // The smallest window the client allows is 820 by 560: what fits there fits anywhere.
        if (std::strcmp(argv[i], "--size") == 0) std::sscanf(argv[i + 1], "%dx%d", &baseWidth, &baseHeight);
    }
    std::filesystem::create_directories(output);

    if (!glfwInit()) {
        std::fprintf(stderr, "GLFW could not start. Without a display, run under xvfb-run.\n");
        return 1;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    const int width = static_cast<int>(static_cast<float>(baseWidth) * scale);
    const int height = static_cast<int>(static_cast<float>(baseHeight) * scale);
    GLFWwindow* window = glfwCreateWindow(width, height, "VeloPlay UI preview", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Could not create an OpenGL window.\n");
        return 1;
    }
    glfwMakeContextCurrent(window);

    // The client uses Segoe UI on Windows. Elsewhere any humanist sans gives a fair impression.
    const char* regular = firstExisting({"C:/Windows/Fonts/segoeui.ttf", "/usr/share/fonts/truetype/open-sans/OpenSans-Regular.ttf",
                                         "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
                                         "/System/Library/Fonts/Supplemental/Arial.ttf"});
    const char* semibold = firstExisting({"C:/Windows/Fonts/seguisb.ttf", "/usr/share/fonts/truetype/open-sans/OpenSans-Semibold.ttf",
                                          "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
                                          "/System/Library/Fonts/Supplemental/Arial Bold.ttf"});

    int failures = 0;
    int cutOff = 0;
    for (const Scene& scene : scenes()) {
        // A fresh ImGui context per scene: no popup or animation state leaks over.
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        ImGui_ImplGlfw_InitForOpenGL(window, false);
        ImGui_ImplOpenGL3_Init("#version 130");

        NoActions actions;
        ui::UiManager manager(actions);
        manager.configure(ui::loadFonts(regular, semibold), scale, language);

        AppConfig config;
        sanitize(config);
        UiModel model;
        model.config = &config;
        model.gpuName = "NVIDIA GeForce RTX 4070";
        // The window as the client has it: without a frame from Windows, with the bar of its own.
        model.ownWindowBar = true;
        model.clock = "21:47";
        scene.setup(model, manager);
        // What the controller would fill in: the stream that is asked for and the screen's rate.
        model.requestedStream = effectiveStream(config, {});
        model.displayHz = 60.0;

        // Every surface and every text that a window cuts off in part gets noted (see ui::ClipAudit).
        ui::context().audit.enabled = true;

        // Enough frames for every spring to settle.
        for (int frame = 0; frame < 90; ++frame) {
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
            io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
            io.DeltaTime = 1.0f / 60.0f;
            // No pointer unless a scene puts one somewhere: the hidden window's own would wander in.
            io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
            if (scene.step) scene.step(model, frame);
            manager.feedNavigation(model);
            // What counts is the picture that gets written: in the first frames sizes are not settled yet.
            ui::context().audit.findings.clear();
            ImGui::NewFrame();
            manager.draw(model);
            ImGui::Render();
            glViewport(0, 0, width, height);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }
        glFinish();

        const std::filesystem::path file = output / (scene.name + ".bmp");
        if (writeBmp(file, width, height)) {
            std::printf("wrote %s\n", file.string().c_str());
        } else {
            std::fprintf(stderr, "could not write %s\n", file.string().c_str());
            ++failures;
        }

        // A menu or a sheet that reaches beyond the window: part of it cannot be seen or clicked.
        for (const ImGuiWindow* open : ImGui::GetCurrentContext()->Windows) {
            if (!open->WasActive || open->Hidden || (open->Flags & ImGuiWindowFlags_ChildWindow)) continue;
            if (!(open->Flags & (ImGuiWindowFlags_Popup | ImGuiWindowFlags_Modal | ImGuiWindowFlags_Tooltip))) continue;
            const ImVec2 min = open->Pos;
            const ImVec2 max(open->Pos.x + open->Size.x, open->Pos.y + open->Size.y);
            if (min.x >= -0.75f && min.y >= -0.75f && max.x <= static_cast<float>(width) + 0.75f &&
                max.y <= static_cast<float>(height) + 0.75f) {
                continue;
            }
            char line[200];
            std::snprintf(line, sizeof(line), "the window '%s' at %.0f,%.0f size %.0f x %.0f reaches beyond the edge", open->Name,
                          min.x, min.y, open->Size.x, open->Size.y);
            ui::context().audit.findings.emplace_back(line);
        }
        for (const std::string& finding : ui::context().audit.findings) {
            std::fprintf(stderr, "CUT OFF in %s (%s): %s\n", scene.name.c_str(), language.c_str(), finding.c_str());
            ++cutOff;
        }

        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }
    if (cutOff > 0) {
        std::fprintf(stderr, "%d things are cut off at an edge. Each one shows as a missing corner or a clipped word.\n", cutOff);
        ++failures;
    }

    glfwDestroyWindow(window);
    glfwTerminate();
    return failures == 0 ? 0 : 1;
}
