#include "ui/UiManager.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <iterator>

#include <imgui.h>

#include "media/FramePacer.hpp"
#include "net/Socket.hpp"
#include "protocol/PsnAccount.hpp"
#include "protocol/Registration.hpp"
#include "ui/Widgets.hpp"

namespace veloplay::ui {

namespace {

constexpr float kColumnWidth = 720.0f;
constexpr float kPageMargin = 32.0f;
constexpr float kSheetPadding = 28.0f;

const ImVec4& statusColor(const ConsoleEntry& console) {
    const Palette& p = palette();
    if (console.waking) return p.standby;
    switch (console.status) {
        case ConsoleStatus::Ready: return p.ready;
        case ConsoleStatus::Standby: return p.standby;
        case ConsoleStatus::Offline: break;
    }
    return p.offline;
}

std::string statusLine(const Strings& t, const ConsoleEntry& console) {
    std::string line;
    if (console.waking) line = t.statusWaking;
    else if (console.status == ConsoleStatus::Ready) line = t.statusReady;
    else if (console.status == ConsoleStatus::Standby) line = t.statusStandby;
    else line = t.statusOffline;

    if (console.status == ConsoleStatus::Ready && !console.runningApp.empty()) {
        line += "  ·  " + format(t.nowRunning, {console.runningApp});
    }
    if (!console.address.empty()) line += "  ·  " + console.address;
    if (!console.registered) line += std::string("  ·  ") + t.notPaired;
    return line;
}

/// "Akku 80 %", with "lädt" on the cable; empty if the controller does not tell.
std::string batteryText(const Strings& t, const input::ControllerInfo& controller) {
    if (controller.batteryPercent < 0) return {};
    return format(controller.charging ? t.batteryCharging : t.batteryValue, {std::to_string(controller.batteryPercent)});
}

std::string controllerLine(const Strings& t, const UiModel& model) {
    if (!model.controllerConnected) return t.controllerNone;
    std::string line = model.controller.name;
    if (model.controller.connection == input::ConnectionType::Usb) line += std::string("  ·  ") + t.viaUsb;
    if (model.controller.connection == input::ConnectionType::Bluetooth) line += std::string("  ·  ") + t.viaBluetooth;
    if (const std::string battery = batteryText(t, model.controller); !battery.empty()) line += "  ·  " + battery;
    return line;
}

std::string noticeText(const Strings& t, const Notice& notice) {
    const char* pattern = "";
    switch (notice.topic) {
        case Notice::Topic::WakeTimedOut: pattern = t.noticeWakeTimedOut; break;
        case Notice::Topic::WakeFailed: pattern = t.noticeWakeFailed; break;
        case Notice::Topic::ConfigSaveFailed: pattern = t.noticeConfigSaveFailed; break;
        case Notice::Topic::ConfigLoadFailed: pattern = t.noticeConfigLoadFailed; break;
        case Notice::Topic::DiscoveryFailed: pattern = t.noticeDiscoveryFailed; break;
        case Notice::Topic::RemoteApiFailed: pattern = t.noticeRemoteApiFailed; break;
        case Notice::Topic::SessionRefused: pattern = t.noticeSessionRefused; break;
        case Notice::Topic::SessionInUse: pattern = t.noticeSessionInUse; break;
        case Notice::Topic::SessionVersionMismatch: pattern = t.noticeSessionVersionMismatch; break;
        case Notice::Topic::SessionConsoleCrashed: pattern = t.noticeSessionConsoleCrashed; break;
        case Notice::Topic::SessionSetupFailed: pattern = t.noticeSessionSetupFailed; break;
        case Notice::Topic::SessionLost: pattern = t.noticeSessionLost; break;
        case Notice::Topic::SleepFailed: pattern = t.noticeSleepFailed; break;
        case Notice::Topic::BitrateLowered: pattern = t.noticeBitrateLowered; break;
    }
    return format(pattern, {notice.detail});
}

const char* pairingFailureText(const Strings& t, ErrorCode code) {
    switch (code) {
        case ErrorCode::Timeout: return t.failTimeout;
        case ErrorCode::Rejected: return t.failRejected;
        case ErrorCode::Network: return t.failNetwork;
        default: return t.failOther;
    }
}

// Vertical rhythm: ImGui's automatic item spacing is switched off inside the
// dashboard and the sheets, and every gap is written out with space(). The
// distances come from one scale: 4, 6, 8, 12, 16, 24, 32.

/// A caption that explains the control above it.
void hint(std::string_view text) {
    space(6.0f);
    paragraph(TextStyle::Caption, text, palette().textSecondary);
}

/// Heading of a group of settings.
void sectionTitle(std::string_view text, bool first = false) {
    if (!first) space(20.0f);
    label(TextStyle::Strong, text);
    space(8.0f);
}

/// Two boxes and an arrow above the picture settings: what the console
/// sends on the left, what reaches the screen on the right. The box that
/// belongs to the open tab is outlined, so it is clear which side the
/// controls below change.
void flowStrip(const Strings& t, const UiModel& model, const AppConfig& config, bool outputSide) {
    const Palette& p = palette();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    const float gap = px(32.0f);
    const float boxWidth = (width - gap) * 0.5f;
    const float pad = px(12.0f);
    const float captionHeight = measure(TextStyle::Caption, "X").y;
    const float valueHeight = measure(TextStyle::Strong, "X").y;
    const float height = pad * 2.0f + captionHeight + px(4.0f) + valueHeight;
    const ImVec2 origin = ImGui::GetCursorScreenPos();

    // With "automatic" the program's own choice, the values from this screen otherwise.
    const StreamSettings& stream = isAutomatic(config) ? model.requestedStream : config.stream;
    const int mbit = static_cast<int>(std::lround(static_cast<float>(stream.bitrateKbps) / 1000.0f));
    const std::string input = std::to_string(stream.height) + "p  ·  " + std::to_string(stream.fps) + " fps  ·  " +
                              format(t.bitrateValue, {std::to_string(mbit)});
    const int factor =
        media::frameGenerationFactorFor(config.graphics.frameGenerationFactor, stream.fps, model.displayHz);
    std::string output = std::to_string(stream.fps * factor) + " fps";
    if (config.graphics.superResolutionLevel > 0) output += std::string("  ·  ") + t.flowUpscaled;
    if (config.graphics.sharpness > 0.0f) output += std::string("  ·  ") + t.flowSharpened;

    const auto box = [&](float x, const char* caption, const std::string& value, bool active) {
        const ImVec2 min(origin.x + x, origin.y);
        const ImVec2 max(min.x + boxWidth, min.y + height);
        draw->AddRectFilled(min, max, toU32(p.control), px(10.0f));
        if (active) draw->AddRect(min, max, toU32(p.accent), px(10.0f), 0, px(1.5f));
        draw->PushClipRect(min, ImVec2(max.x - pad * 0.5f, max.y), true);
        drawText(draw, ImVec2(min.x + pad, min.y + pad), TextStyle::Caption, active ? p.textSecondary : p.textTertiary,
                 caption);
        drawText(draw, ImVec2(min.x + pad, min.y + pad + captionHeight + px(4.0f)), TextStyle::Strong,
                 active ? p.text : p.textSecondary, value);
        draw->PopClipRect();
    };
    box(0.0f, t.flowInput, input, !outputSide);
    box(boxWidth + gap, t.flowOutput, output, outputSide);

    // The arrow between them.
    const ImU32 ink = toU32(p.textTertiary);
    const float midY = origin.y + height * 0.5f;
    const float left = origin.x + boxWidth + px(9.0f);
    const float right = origin.x + boxWidth + gap - px(9.0f);
    const float head = px(4.0f);
    draw->AddLine(ImVec2(left, midY), ImVec2(right, midY), ink, px(1.5f));
    draw->AddLine(ImVec2(right - head, midY - head), ImVec2(right, midY), ink, px(1.5f));
    draw->AddLine(ImVec2(right - head, midY + head), ImVec2(right, midY), ink, px(1.5f));

    ImGui::Dummy(ImVec2(width, height));
}

struct BannerButton {
    const char* label;
    ButtonKind kind;
};

/// A full-width row with a coloured dot, a message and buttons on the right.
/// Returns the index of the button that was clicked, or -1. A `progress`
/// between 0 and 1 draws a thin bar along the bottom edge. `detail` is a
/// technical second line in small print.
int banner(const ImVec4& tone, const std::string& message, std::initializer_list<BannerButton> buttons,
           float progress = -1.0f, const std::string& detail = {}) {
    const Palette& p = palette();
    const float width = ImGui::GetContentRegionAvail().x;
    const float pad = px(20.0f);
    const float textX = pad + px(22.0f);

    float buttonsWidth = 0.0f;
    for (const BannerButton& entry : buttons) buttonsWidth += buttonSize(entry.label).x + px(8.0f);
    const float textWidth = width - textX - buttonsWidth - px(12.0f);

    pushText(TextStyle::Body);
    const float lineHeight = ImGui::GetFontSize();
    const ImVec2 messageSize = ImGui::CalcTextSize(message.c_str(), nullptr, false, textWidth);
    popText();
    ImVec2 detailSize(0.0f, 0.0f);
    if (!detail.empty()) {
        pushText(TextStyle::Caption);
        detailSize = ImGui::CalcTextSize(detail.c_str(), nullptr, false, textWidth);
        popText();
    }
    const float detailGap = detail.empty() ? 0.0f : px(4.0f);
    const ImVec2 textSize(messageSize.x, messageSize.y + detailGap + detailSize.y);
    const float height = std::max(textSize.y, px(36.0f)) + 2.0f * px(12.0f);
    const float textY = (height - textSize.y) * 0.5f;

    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, ImVec2(min.x + width, min.y + height), toU32(p.surface), px(14.0f));
    // The dot sits on the first line of text, however many lines follow.
    statusDot(draw, ImVec2(min.x + pad + px(5.0f), min.y + textY + lineHeight * 0.5f + px(1.0f)), tone, false);

    if (progress >= 0.0f) {
        const float inset = px(14.0f);
        const float y = min.y + height - px(3.0f);
        const float full = width - 2.0f * inset;
        draw->AddRectFilled(ImVec2(min.x + inset, y), ImVec2(min.x + inset + full, y + px(3.0f)), toU32(p.control), px(1.5f));
        draw->AddRectFilled(ImVec2(min.x + inset, y),
                            ImVec2(min.x + inset + full * std::clamp(progress, 0.0f, 1.0f), y + px(3.0f)),
                            toU32(p.accentHover), px(1.5f));
    }

    ImGui::SetCursorScreenPos(ImVec2(min.x + textX, min.y + textY));
    pushText(TextStyle::Body);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textWidth);
    ImGui::TextUnformatted(message.c_str());
    ImGui::PopTextWrapPos();
    popText();
    if (!detail.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(min.x + textX, min.y + textY + messageSize.y + detailGap));
        pushText(TextStyle::Caption);
        ImGui::PushStyleColor(ImGuiCol_Text, p.textTertiary);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textWidth);
        ImGui::TextUnformatted(detail.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        popText();
    }

    int clicked = -1;
    int index = 0;
    float x = min.x + width - px(8.0f) - buttonsWidth + px(4.0f);
    for (const BannerButton& entry : buttons) {
        ImGui::SetCursorScreenPos(ImVec2(x, min.y + (height - px(36.0f)) * 0.5f));
        if (button(entry.label, entry.kind)) clicked = index;
        x += buttonSize(entry.label).x + px(8.0f);
        ++index;
    }

    ImGui::SetCursorScreenPos(min);
    ImGui::Dummy(ImVec2(width, height));
    return clicked;
}

const ImVec4& qualityColor(protocol::LinkQuality quality) {
    const Palette& p = palette();
    switch (quality) {
        case protocol::LinkQuality::Excellent:
        case protocol::LinkQuality::Good: return p.ready;
        case protocol::LinkQuality::Fair: return p.standby;
        case protocol::LinkQuality::Poor: return p.danger;
        case protocol::LinkQuality::Unknown: break;
    }
    return p.offline;
}

/// Escape on the keyboard, Circle on the controller.
bool cancelPressed() {
    return ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
}

void pushExplicitSpacing() { ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8.0f), 0.0f)); }
void popExplicitSpacing() { ImGui::PopStyleVar(); }

/// Places the cursor so that content of `width` ends at the right edge.
void alignRight(float width) {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - width));
}

// Defined further down, with the settings for keys and mouse.
std::string keyLabel(const Strings& t, int code);
std::string controlLabel(const Strings& t, input::PadControl control);

// Left column: what the left hand does on a controller. Right column: the right hand.
constexpr std::array<input::PadControl, 13> kLeftHand = {
    input::PadControl::L2,            input::PadControl::L1,             input::PadControl::LeftStickUp,
    input::PadControl::LeftStickLeft, input::PadControl::LeftStickDown,  input::PadControl::LeftStickRight,
    input::PadControl::L3,            input::PadControl::DpadUp,         input::PadControl::DpadLeft,
    input::PadControl::DpadDown,      input::PadControl::DpadRight,      input::PadControl::Create,
    input::PadControl::Touchpad,
};
constexpr std::array<input::PadControl, 13> kRightHand = {
    input::PadControl::R2,            input::PadControl::R1,             input::PadControl::Triangle,
    input::PadControl::Circle,        input::PadControl::Cross,          input::PadControl::Square,
    input::PadControl::R3,            input::PadControl::Options,        input::PadControl::Ps,
    input::PadControl::RightStickUp,  input::PadControl::RightStickLeft, input::PadControl::RightStickDown,
    input::PadControl::RightStickRight,
};

} // namespace

UiManager::UiManager(IUiActions& actions) : actions_(actions), text_(&strings(Language::English)) {}

void UiManager::configure(const Fonts& fonts, float scale, std::string_view systemLanguage) {
    Context& ctx = context();
    ctx.fonts = fonts;
    ctx.scale = scale;
    systemLanguage_ = std::string(systemLanguage);
    applyTheme(scale);
}

void UiManager::feedNavigation(const UiModel& model) {
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    if (!model.controllerConnected) return;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    // While connected the controller belongs to the console. A press of Cross
    // must not also click a button in this window.
    if (model.state == AppState::Streaming) return;

    using input::Button;
    const input::ControllerState& s = model.controllerState;
    io.AddKeyEvent(ImGuiKey_GamepadFaceDown, s.pressed(Button::Cross));
    io.AddKeyEvent(ImGuiKey_GamepadFaceRight, s.pressed(Button::Circle));
    io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, s.pressed(Button::Square));
    io.AddKeyEvent(ImGuiKey_GamepadFaceUp, s.pressed(Button::Triangle));
    io.AddKeyEvent(ImGuiKey_GamepadDpadUp, s.pressed(Button::DpadUp));
    io.AddKeyEvent(ImGuiKey_GamepadDpadDown, s.pressed(Button::DpadDown));
    io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, s.pressed(Button::DpadLeft));
    io.AddKeyEvent(ImGuiKey_GamepadDpadRight, s.pressed(Button::DpadRight));
    io.AddKeyEvent(ImGuiKey_GamepadL1, s.pressed(Button::L1));
    io.AddKeyEvent(ImGuiKey_GamepadR1, s.pressed(Button::R1));
    io.AddKeyEvent(ImGuiKey_GamepadStart, s.pressed(Button::Options));

    const auto axis = [&](ImGuiKey negative, ImGuiKey positive, std::int16_t value) {
        const float v = static_cast<float>(value) / 32767.0f;
        io.AddKeyAnalogEvent(negative, v < -0.3f, std::max(0.0f, -v));
        io.AddKeyAnalogEvent(positive, v > 0.3f, std::max(0.0f, v));
    };
    axis(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, s.leftX);
    axis(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, s.leftY);
}

void UiManager::openPairing(std::optional<std::string> address) {
    sheet_ = Sheet::Pairing;
    ++sheetGeneration_;
    pairingPin_.clear();
    accountPrefilled_ = false;
    if (address) {
        pairingAddress_ = std::move(*address);
        pairingStep_ = PairingStep::Account;
    } else {
        pairingAddress_.clear();
        pairingStep_ = PairingStep::Console;
    }
}

void UiManager::openSettings(SettingsTab tab) {
    sheet_ = Sheet::Settings;
    ++sheetGeneration_;
    settingsTab_ = tab;
    editedGameChosen_ = false; // the key layout shown is picked anew: the one in use now
}

void UiManager::openConnection(std::string_view hostId) {
    sheet_ = Sheet::Connection;
    ++sheetGeneration_;
    adviceApplied_ = false;
    actions_.startConnectionTest(hostId);
}

void UiManager::closeSheet() { sheet_ = Sheet::None; }

std::string UiManager::number(float value, int decimals) const {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, static_cast<double>(value));
    std::string result = buffer;
    // German writes the decimal separator as a comma.
    if (german_) std::replace(result.begin(), result.end(), '.', ',');
    return result;
}

void UiManager::setPairingInputs(std::string address, std::string account, std::string pin) {
    pairingAddress_ = std::move(address);
    pairingAccount_ = std::move(account);
    pairingPin_ = std::move(pin);
    accountPrefilled_ = true;
}

void UiManager::draw(const UiModel& model) {
    Context& ctx = context();
    const std::string_view languageSetting = model.config ? std::string_view(model.config->language) : "auto";
    const Language language = resolveLanguage(languageSetting, systemLanguage_);
    german_ = language == Language::German;
    text_ = &strings(language);
    ctx.text = text_;
    ctx.animator.setReducedMotion(model.reducedMotion);

    pictureOwnsInput_ = false;
    const bool inSession = model.state == AppState::Connecting || model.state == AppState::Streaming ||
                           model.state == AppState::Reconnecting || model.state == AppState::Disconnecting;
    if (inSession) {
        // Once the picture is there it owns the window. Esc brings the menu up.
        const bool presenting = model.state == AppState::Streaming && model.stream && model.stream->presenting;
        // No sheet survives into a session; it would reappear over the
        // dashboard later. The one exception is the settings sheet opened
        // from the menu over the running picture.
        const bool settingsInGame = settingsFromGame_ && presenting && sheet_ == Sheet::Settings;
        if (!settingsInGame) {
            sheet_ = Sheet::None;
            openedSheet_ = Sheet::None;
            settingsFromGame_ = false;
        }
        if (!presenting) pauseMenu_ = false;
        else if (!settingsInGame && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) pauseMenu_ = !pauseMenu_;

        if (settingsInGame) {
            drawSheets(model); // closing it leads back to the menu
        } else if (!presenting || pauseMenu_) {
            drawSession(model);
        } else if (model.session.connectedSeconds < 8) {
            // A reminder of the keys, gone after a few seconds.
            const Palette& p = palette();
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            const ImVec2 size = measure(TextStyle::Caption, text_->streamKeys);
            const ImVec2 min(viewport->Pos.x + (viewport->Size.x - size.x) * 0.5f - px(12.0f),
                             viewport->Pos.y + viewport->Size.y - size.y - px(36.0f));
            ImDrawList* draw = ImGui::GetForegroundDrawList();
            draw->AddRectFilled(min, ImVec2(min.x + size.x + px(24.0f), min.y + size.y + px(12.0f)),
                                toU32(p.background, 0.80f), px(10.0f));
            drawText(draw, ImVec2(min.x + px(12.0f), min.y + px(6.0f)), TextStyle::Caption, p.text, text_->streamKeys);
        }
        // The menu names the figures that matter itself; the display would only lie across it.
        const bool menuOpen = presenting && pauseMenu_ && !settingsInGame;
        // Every frame, so the menu fades in each time it is opened.
        menuShown_ = context().animator.to(static_cast<ImGuiID>(0x5A1E7100), menuOpen ? 1.0f : 0.0f, 0.16f, 0.0f);
        if (model.stream && !menuOpen) drawStreamOverlay(model);
        // Over the picture only; the screens before and around it say more than a note could.
        if (presenting && !pauseMenu_ && !settingsInGame) {
            drawStreamAlerts(model);
            drawUpscalingNote(model);
            drawKeyOverlay(model);
        } else {
            upscalingNoteFor_ = 0.0;
            keyOverlay_ = false; // a menu is open; the layout does not come back by itself
        }
        pictureOwnsInput_ = presenting && !pauseMenu_ && !settingsInGame;
        return;
    }
    if (std::exchange(settingsFromGame_, false)) {
        // Settings opened in the game do not follow onto the dashboard.
        sheet_ = Sheet::None;
        openedSheet_ = Sheet::None;
    }
    pauseMenu_ = false;
    loginPin_.clear();
    drawDashboard(model);
    drawSheets(model);
}

// ---- dashboard ---------------------------------------------------------

void UiManager::drawDashboard(const UiModel& model) {
    const Strings& t = *text_;
    const Palette& p = palette();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::Begin("dashboard", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    // One centred column. Wide windows get calm margins instead of stretched rows.
    const float margin = px(kPageMargin);
    const float column = std::min(px(kColumnWidth), viewport->Size.x - 2.0f * margin);
    ImGui::SetCursorPos(ImVec2(std::floor((viewport->Size.x - column) * 0.5f), 0.0f));
    ImGui::BeginChild("column", ImVec2(column, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    pushExplicitSpacing();

    space(28.0f);
    drawHeader(model);
    space(24.0f);
    drawUpdateBanner(model);
    drawNotices(model);

    if (model.consoles.empty()) {
        drawEmptyState(model);
    } else {
        for (int i = 0; i < static_cast<int>(model.consoles.size()); ++i) {
            drawConsoleRow(model, model.consoles[i], i);
            space(10.0f);
        }
        if (edgeButton(t.addConsole)) openPairing();
        space(24.0f);
        drawModePicker(model);
    }

    // Footer: what this build can do, and on which graphics card it runs.
    space(32.0f);
    hairline();
    space(12.0f);
    paragraph(TextStyle::Caption, format(t.previewNote, {VELOPLAY_VERSION}), p.textTertiary);
    if (!model.gpuName.empty()) {
        space(4.0f);
        label(TextStyle::Caption, format(t.graphicsCard, {model.gpuName}), p.textTertiary);
    }
    space(24.0f);

    popExplicitSpacing();
    ImGui::EndChild();
    ImGui::End();
}

void UiManager::drawHeader(const UiModel& model) {
    const Strings& t = *text_;
    const Palette& p = palette();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float height = px(36.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const float mark = px(28.0f);
    drawLogo(draw, ImVec2(start.x, start.y + (height - mark) * 0.5f), mark);
    const ImVec2 nameSize = measure(TextStyle::Title, "VeloPlay");
    drawText(draw, ImVec2(start.x + mark + px(10.0f), start.y + (height - nameSize.y) * 0.5f), TextStyle::Title, p.text,
             "VeloPlay");

    const ImVec2 settingsSize = buttonSize(t.settings);
    ImGui::SetCursorScreenPos(ImVec2(start.x + width - settingsSize.x, start.y));
    if (button(t.settings, ButtonKind::Secondary)) openSettings();

    // Controller status sits left of the settings button.
    const std::string controller = controllerLine(t, model);
    const ImVec2 controllerSize = measure(TextStyle::Body, controller);
    const float textX = start.x + width - settingsSize.x - px(20.0f) - controllerSize.x;
    statusDot(draw, ImVec2(textX - px(14.0f), start.y + height * 0.5f + px(1.0f)),
              model.controllerConnected ? p.ready : p.offline, false);
    drawText(draw, ImVec2(textX, start.y + (height - controllerSize.y) * 0.5f), TextStyle::Body,
             model.controllerConnected ? p.text : p.textSecondary, controller);

    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(width, height));
}

void UiManager::drawUpdateBanner(const UiModel& model) {
    if (!model.updaterAvailable) return;
    const Strings& t = *text_;
    const Palette& p = palette();
    const update::UpdateStatus& status = model.update;
    const std::string version = status.latest.toString();
    using Phase = update::UpdateStatus::Phase;

    ImGui::PushID("update");
    switch (status.phase) {
        case Phase::Available: {
            const int clicked = banner(p.accentHover, format(t.updateAvailable, {version}),
                                       {{t.updateNotes, ButtonKind::Quiet}, {t.updateInstall, ButtonKind::Primary}});
            if (clicked == 0) actions_.openHelp("release-notes");
            if (clicked == 1) actions_.installUpdate();
            break;
        }
        case Phase::Downloading: {
            const std::string percent = std::to_string(static_cast<int>(std::lround(status.progress * 100.0f)));
            banner(p.accentHover, format(t.updateDownloading, {version, percent}), {}, status.progress);
            break;
        }
        case Phase::Installing:
            banner(p.accentHover, format(t.updateInstalling, {version}), {}, 1.0f);
            break;
        case Phase::RestartNeeded:
            if (banner(p.ready, format(t.updateRestart, {version}), {{t.restartNow, ButtonKind::Primary}}) == 0) {
                actions_.restart();
            }
            break;
        case Phase::Failed: {
            // A check that failed quietly at startup is not shown here; the
            // settings say more. This is for an installation the user started.
            if (!status.failedDuringInstall) {
                ImGui::PopID();
                return;
            }
            const bool retryHelps = status.error == update::UpdateError::Checksum ||
                                    status.error == update::UpdateError::Network;
            const char* message = status.error == update::UpdateError::Checksum      ? t.updateFailChecksum
                                  : status.error == update::UpdateError::Network     ? t.updateFailNetwork
                                  : status.error == update::UpdateError::NotWritable ? t.updateFailWrite
                                                                                     : t.updateFailArchive;
            if (retryHelps) {
                if (banner(p.danger, message, {{t.retry, ButtonKind::Secondary}}) == 0) actions_.installUpdate();
            } else {
                if (banner(p.danger, message, {{t.openDownloads, ButtonKind::Secondary}}) == 0) actions_.openHelp("releases");
            }
            break;
        }
        default:
            ImGui::PopID();
            return;
    }
    ImGui::PopID();
    space(16.0f);
}

void UiManager::drawNotices(const UiModel& model) {
    const Strings& t = *text_;
    const Palette& p = palette();
    for (std::size_t i = 0; i < model.notices.size(); ++i) {
        const Notice& notice = model.notices[i];
        ImGui::PushID(static_cast<int>(i));
        const ImVec4& tone = notice.kind == Notice::Kind::Error ? p.danger : p.standby;
        if (banner(tone, noticeText(t, notice), {{t.close, ButtonKind::Quiet}}, -1.0f, notice.technical) == 0) {
            actions_.dismissNotice(i);
        }
        ImGui::PopID();
        space(10.0f);
    }
    if (!model.notices.empty()) space(6.0f);
}

void UiManager::drawEmptyState(const UiModel& model) {
    const Strings& t = *text_;
    const Palette& p = palette();
    space(32.0f);

    if (model.discoveryRunning) {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float titleHeight = measure(TextStyle::Display, t.searchTitle).y;
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + (titleHeight - px(22.0f)) * 0.5f + px(1.0f)));
        spinner(11.0f);
        ImGui::SetCursorScreenPos(ImVec2(start.x + px(34.0f), start.y));
    }
    label(TextStyle::Display, t.searchTitle);
    space(8.0f);
    paragraph(TextStyle::Body, t.searchBody, p.textSecondary);
    space(14.0f);

    // Conditions that all have to hold, so a short list is the honest form.
    for (const char* line : {t.searchCheckPower, t.searchCheckNetwork, t.searchCheckRemotePlay, t.searchCheckFirewall}) {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        pushText(TextStyle::Body);
        const float lineHeight = ImGui::GetFontSize();
        popText();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(start.x + px(5.0f), start.y + lineHeight * 0.5f + px(1.0f)),
                                                    px(2.0f), toU32(p.textTertiary));
        ImGui::Indent(px(18.0f));
        paragraph(TextStyle::Body, line, p.text);
        ImGui::Unindent(px(18.0f));
        space(8.0f);
    }
    space(16.0f);
    if (button(t.enterAddress, ButtonKind::Secondary)) openPairing();
}

void UiManager::drawConsoleRow(const UiModel& model, const ConsoleEntry& console, int index) {
    const Strings& t = *text_;
    const Palette& p = palette();
    ImGui::PushID(index);

    const bool needsRepair = console.registered && !console.credentialsValid;
    const float width = ImGui::GetContentRegionAvail().x;
    const float pad = px(20.0f);
    const float textX = pad + px(22.0f);

    // Left: status dot, name, one line of facts. The block is centred in the row.
    const ImVec2 nameSize = measure(TextStyle::Display, console.name);
    const float factsHeight = measure(TextStyle::Body, "X").y;
    const float blockHeight = nameSize.y + px(4.0f) + factsHeight;
    const float baseHeight = px(84.0f);
    const float warningHeight = needsRepair ? measure(TextStyle::Caption, "X").y + px(14.0f) : 0.0f;
    const float height = baseHeight + warningHeight;

    const ImVec2 min = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, ImVec2(min.x + width, min.y + height), toU32(p.surface), px(14.0f));

    const float nameY = min.y + (baseHeight - blockHeight) * 0.5f;
    statusDot(draw, ImVec2(min.x + pad + px(5.0f), nameY + nameSize.y * 0.5f + px(1.0f)), statusColor(console),
              console.waking);
    drawText(draw, ImVec2(min.x + textX, nameY), TextStyle::Display, p.text, console.name);
    drawText(draw, ImVec2(min.x + textX, nameY + nameSize.y + px(4.0f)), TextStyle::Body, p.textSecondary,
             statusLine(t, console));
    if (needsRepair) {
        drawText(draw, ImVec2(min.x + textX, min.y + baseHeight - px(4.0f)), TextStyle::Caption, p.standby,
                 t.keysUnreadable);
    }

    // Right: the action that makes sense for this console right now.
    const float buttonY = min.y + (baseHeight - px(36.0f)) * 0.5f;
    float right = min.x + width - px(12.0f);
    const auto place = [&](float buttonWidth) {
        right -= buttonWidth;
        ImGui::SetCursorScreenPos(ImVec2(right, buttonY));
        right -= px(8.0f);
    };

    const bool hasMenu = console.registered || !console.address.empty();
    if (hasMenu) {
        place(px(36.0f));
        if (moreButton("more")) {
            forgetCandidate_.clear();
            ImGui::OpenPopup("console-menu");
        }
    } else {
        right -= px(8.0f); // same right margin as rows that end in a menu button
    }

    if (!console.registered || needsRepair) {
        const char* text = needsRepair ? t.pairAgain : t.pair;
        place(buttonSize(text).x);
        if (button(text, ButtonKind::Primary)) {
            openPairing(console.address.empty() ? std::nullopt : std::optional<std::string>(console.address));
        }
    } else {
        // Connecting is what the row is for. A console in rest mode is woken on
        // the way; one that does not answer at all can still be tried, because
        // discovery does not cross every network.
        const bool idle = model.state == AppState::Idle || model.state == AppState::Error;
        const bool canConnect = idle && !console.address.empty();
        place(buttonSize(t.connect).x);
        if (button(t.connect, ButtonKind::Primary, ImVec2(0, 0), canConnect)) actions_.connect(console.hostId);
        if (console.status != ConsoleStatus::Ready) {
            const bool canWake = !console.waking && idle;
            place(buttonSize(t.wake).x);
            if (button(t.wake, ButtonKind::Secondary, ImVec2(0, 0), canWake)) actions_.wake(console.hostId);
        }
    }

    // Row menu with the one destructive action, confirmed in place.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(12.0f), px(12.0f)));
    if (ImGui::BeginPopup("console-menu")) {
        if (forgetCandidate_ != console.hostId) {
            const float menuWidth = std::max(buttonSize(t.checkConnection).x, buttonSize(t.forget).x);
            if (!console.address.empty() && button(t.checkConnection, ButtonKind::Secondary, ImVec2(menuWidth, 0))) {
                openConnection(console.hostId);
                ImGui::CloseCurrentPopup();
            }
            if (console.registered) {
                if (!console.address.empty()) space(8.0f);
                if (button(t.forget, ButtonKind::Danger, ImVec2(menuWidth, 0))) forgetCandidate_ = console.hostId;
            }
        } else {
            label(TextStyle::Body, format(t.forgetQuestion, {console.name}));
            space(12.0f);
            if (button(t.forget, ButtonKind::Danger)) {
                actions_.forgetConsole(console.hostId);
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (button(t.cancel, ButtonKind::Quiet)) ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();

    ImGui::SetCursorScreenPos(min);
    ImGui::Dummy(ImVec2(width, height));
    ImGui::PopID();
}

void UiManager::drawModePicker(const UiModel& model) {
    if (!model.config) return;
    const Strings& t = *text_;
    const Palette& p = palette();
    const std::string& active = model.config->activeProfile;

    label(TextStyle::Strong, t.modeTitle);
    space(8.0f);

    const bool custom = active == kProfileCustom;
    const std::array<const char*, 4> labels = {t.modeAuto, t.modeSmooth, t.modeNative, t.modeCustom};
    const std::array<std::string_view, 3> ids = {kProfileAuto, kProfileSmooth30, kProfileNative60};
    const int count = custom ? 4 : 3; // "custom" only appears while it is in effect
    const int selected = active == kProfileAuto ? 0 : active == kProfileSmooth30 ? 1 : active == kProfileNative60 ? 2 : 3;
    const int chosen = segmented("mode", std::span(labels.data(), static_cast<std::size_t>(count)), selected);
    if (chosen != selected && chosen < 3) actions_.selectProfile(ids[static_cast<std::size_t>(chosen)]);

    space(10.0f);
    const std::array<const char*, 4> hints = {t.modeAutoHint, t.modeSmoothHint, t.modeNativeHint, t.modeCustomHint};
    const char* explanation = hints[static_cast<std::size_t>(selected)];
    paragraph(TextStyle::Body, explanation, p.textSecondary);
}

// ---- sheets ------------------------------------------------------------

bool UiManager::beginSheet(const char* id, float width) {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    Animator& animator = context().animator;

    // The sheet fades in and rises a few pixels. Input is never blocked while it moves.
    const ImGuiID animationId = ImGui::GetID(id) + static_cast<ImGuiID>(sheetGeneration_);
    const float appear = animator.to(animationId, 1.0f, 0.28f, 0.0f);
    const float sheetWidth = std::min(px(width), viewport->Size.x - px(32.0f));

    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                   viewport->Pos.y + viewport->Size.y * 0.5f + (1.0f - appear) * px(14.0f)),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(sheetWidth, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(sheetWidth, 0.0f), ImVec2(sheetWidth, viewport->Size.y - px(32.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::clamp(appear, 0.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(kSheetPadding), px(kSheetPadding)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, px(18.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, palette().surfaceRaised);
    const bool open = ImGui::BeginPopupModal(id, nullptr,
                                             ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (open) {
        pushExplicitSpacing();
    } else {
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(4);
    }
    return open;
}

void UiManager::endSheet() {
    popExplicitSpacing();
    ImGui::EndPopup();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(4);
}

void UiManager::drawSheets(const UiModel& model) {
    const char* ids[] = {"", "pairing-sheet", "settings-sheet", "connection-sheet"};
    if (sheet_ != openedSheet_) {
        openedSheet_ = sheet_;
        if (sheet_ != Sheet::None) ImGui::OpenPopup(ids[static_cast<int>(sheet_)]);
    }
    if (sheet_ == Sheet::Pairing) drawPairing(model);
    if (sheet_ == Sheet::Settings) drawSettings(model);
    if (sheet_ == Sheet::Connection) drawConnection(model);
}

void UiManager::drawPairing(const UiModel& model) {
    const Strings& t = *text_;
    const Palette& p = palette();
    if (!beginSheet("pairing-sheet", 520.0f)) return;

    // The account ID from an earlier pairing saves the user a step of typing.
    if (!accountPrefilled_ && model.config) {
        if (pairingAccount_.empty()) pairingAccount_ = model.config->accountId;
        accountPrefilled_ = true;
    }

    const bool busy = model.pairing.phase != PairingStatus::Phase::Idle;
    if (busy) pairingStep_ = PairingStep::Progress;
    else if (pairingStep_ == PairingStep::Progress) pairingStep_ = PairingStep::Pin;

    if (pairingStep_ == PairingStep::Progress) {
        drawPairingProgress(model);
        endSheet();
        return;
    }

    const int stepNumber = pairingStep_ == PairingStep::Console ? 1 : pairingStep_ == PairingStep::Account ? 2 : 3;
    label(TextStyle::Caption, format(t.stepOf, {std::to_string(stepNumber)}), p.textSecondary);
    space(6.0f);

    const auto address = net::parseIpv4(trim(pairingAddress_));
    const auto account = protocol::PsnAccountId::parse(pairingAccount_);
    const auto pin = protocol::parsePin(pairingPin_);
    bool canContinue = false;
    bool submit = false;

    if (pairingStep_ == PairingStep::Console) {
        label(TextStyle::Title, t.stepConsoleTitle);
        space(8.0f);
        paragraph(TextStyle::Body, t.stepConsoleBody, p.textSecondary);
        space(16.0f);

        bool anyListed = false;
        for (std::size_t i = 0; i < model.consoles.size(); ++i) {
            const ConsoleEntry& console = model.consoles[i];
            if (console.address.empty() || console.status == ConsoleStatus::Offline) continue;
            anyListed = true;
            ImGui::PushID(static_cast<int>(i));
            const bool chosen = trim(pairingAddress_) == console.address;
            const float width = ImGui::GetContentRegionAvail().x;
            const float height = px(48.0f);
            const ImVec2 min = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("pick", ImVec2(width, height))) pairingAddress_ = console.address;
            const float hover = context().animator.to(ImGui::GetItemID(), ImGui::IsItemHovered() ? 1.0f : 0.0f, 0.16f);
            const float picked = context().animator.to(ImGui::GetItemID() + 1, chosen ? 1.0f : 0.0f, 0.18f);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(min, ImVec2(min.x + width, min.y + height), toU32(mix(p.control, p.controlHover, hover)),
                                px(10.0f));
            if (picked > 0.01f) {
                draw->AddRect(min, ImVec2(min.x + width, min.y + height), toU32(p.accentHover, picked), px(10.0f), 0,
                              px(2.0f));
            }
            statusDot(draw, ImVec2(min.x + px(20.0f), min.y + height * 0.5f + px(1.0f)), statusColor(console), false);
            const ImVec2 nameSize = measure(TextStyle::Strong, console.name);
            drawText(draw, ImVec2(min.x + px(36.0f), min.y + (height - nameSize.y) * 0.5f), TextStyle::Strong, p.text,
                     console.name);
            const ImVec2 addressSize = measure(TextStyle::Body, console.address);
            drawText(draw, ImVec2(min.x + width - px(16.0f) - addressSize.x, min.y + (height - addressSize.y) * 0.5f),
                     TextStyle::Body, p.textSecondary, console.address);
            space(8.0f);
            ImGui::PopID();
        }
        if (!anyListed) {
            paragraph(TextStyle::Body, t.stepConsoleNone, p.textTertiary);
            space(8.0f);
        }
        space(8.0f);

        const bool typed = !trim(pairingAddress_).empty();
        submit = textField(t.addressLabel, pairingAddress_, "192.168.1.20",
                           !typed ? FieldState::Neutral : address ? FieldState::Valid : FieldState::Invalid,
                           typed && !address ? t.addressInvalid : "", ImGuiInputTextFlags_EnterReturnsTrue);
        canContinue = address.has_value();
    } else if (pairingStep_ == PairingStep::Account) {
        label(TextStyle::Title, t.stepAccountTitle);
        space(8.0f);
        paragraph(TextStyle::Body, t.stepAccountBody, p.textSecondary);
        space(16.0f);

        const bool typed = !trim(pairingAccount_).empty();
        submit = textField(t.accountLabel, pairingAccount_, "AbCdEfGhIjk=",
                           !typed ? FieldState::Neutral : account ? FieldState::Valid : FieldState::Invalid,
                           typed && !account ? t.accountInvalid : t.accountPrivacy, ImGuiInputTextFlags_EnterReturnsTrue);
        // The address Sony sends the browser to can be pasted here as well;
        // it is traded for the account ID right away.
        if (protocol::psnCodeFromRedirect(pairingAccount_)) {
            actions_.submitAccountRedirect(pairingAccount_);
            pairingAccount_.clear();
        }
        const AccountLookup& lookup = model.accountLookup;
        if (lookup.phase == AccountLookup::Phase::Succeeded) {
            pairingAccount_ = lookup.accountId;
            actions_.dismissAccountLookup();
        }

        space(12.0f);
        const bool busy = lookup.phase == AccountLookup::Phase::SigningIn || lookup.phase == AccountLookup::Phase::Fetching;
        if (busy) {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            spinner(8.0f);
            ImGui::SetCursorScreenPos(ImVec2(start.x + px(26.0f), start.y));
            paragraph(TextStyle::Caption,
                      lookup.phase == AccountLookup::Phase::Fetching ? t.accountFetching : t.accountSigningIn,
                      p.textSecondary);
            space(8.0f);
            if (button(t.cancel, ButtonKind::Quiet)) actions_.dismissAccountLookup();
        } else if (model.accountLoginAvailable) {
            if (button(t.accountSignIn, ButtonKind::Secondary)) actions_.startAccountLogin();
            space(6.0f);
            if (lookup.phase == AccountLookup::Phase::Failed) {
                paragraph(TextStyle::Caption, t.accountLookupFailed, p.danger);
                if (!lookup.errorDetail.empty()) paragraph(TextStyle::Caption, lookup.errorDetail, p.textTertiary);
            } else {
                paragraph(TextStyle::Caption, t.accountSignInHint, p.textSecondary);
            }
        }
        space(8.0f);
        if (edgeButton(t.accountHelp, ButtonKind::Link)) actions_.openHelp("account-id");
        canContinue = account.has_value();
    } else {
        label(TextStyle::Title, t.stepPinTitle);
        space(12.0f);
        int number = 1;
        for (const char* line : {t.stepPinOpen, t.stepPinEnter}) {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            drawText(ImGui::GetWindowDrawList(), start, TextStyle::Strong, p.textSecondary, std::to_string(number++) + ".");
            ImGui::Indent(px(22.0f));
            paragraph(TextStyle::Body, line, p.text);
            ImGui::Unindent(px(22.0f));
            space(8.0f);
        }
        space(8.0f);

        const bool typed = !trim(pairingPin_).empty();
        // Only complain once the user has typed enough digits to have meant it.
        const bool complain = typed && !pin && pairingPin_.size() >= 8;
        submit = textField(t.pinLabel, pairingPin_, "1234 5678",
                           !typed ? FieldState::Neutral : pin ? FieldState::Valid : complain ? FieldState::Invalid
                                                                                              : FieldState::Neutral,
                           complain ? t.pinInvalid : "",
                           ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CharsDecimal, px(190.0f),
                           TextStyle::Title);
        canContinue = pin.has_value();
    }

    // Footer: back or cancel on the left, the way forward on the right.
    space(24.0f);
    const char* leftText = pairingStep_ == PairingStep::Console ? t.cancel : t.back;
    const char* rightText = pairingStep_ == PairingStep::Pin ? t.pair : t.next;
    const bool goBack = edgeButton(leftText);
    ImGui::SameLine();
    alignRight(buttonSize(rightText).x);
    const bool goOn = button(rightText, ButtonKind::Primary, ImVec2(0, 0), canContinue) || (submit && canContinue);

    const bool escape = cancelPressed();
    if (goBack || escape) {
        if (pairingStep_ == PairingStep::Console || escape) closeSheet();
        else pairingStep_ = pairingStep_ == PairingStep::Pin ? PairingStep::Account : PairingStep::Console;
    } else if (goOn) {
        if (pairingStep_ == PairingStep::Console) pairingStep_ = PairingStep::Account;
        else if (pairingStep_ == PairingStep::Account) pairingStep_ = PairingStep::Pin;
        else actions_.startPairing(pairingAddress_, pairingAccount_, pairingPin_);
    }
    if (sheet_ == Sheet::None) ImGui::CloseCurrentPopup();
    endSheet();
}

void UiManager::drawPairingProgress(const UiModel& model) {
    const Strings& t = *text_;
    const Palette& p = palette();
    const float titleHeight = measure(TextStyle::Title, "X").y;

    switch (model.pairing.phase) {
        case PairingStatus::Phase::Running: {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + (titleHeight - px(22.0f)) * 0.5f + px(1.0f)));
            spinner(11.0f);
            ImGui::SetCursorScreenPos(ImVec2(start.x + px(34.0f), start.y));
            label(TextStyle::Title, t.pairingRunning);
            space(8.0f);
            paragraph(TextStyle::Body, t.pairingRunningBody, p.textSecondary);
            space(24.0f);
            if (edgeButton(t.cancel)) actions_.cancelPairing();
            break;
        }
        case PairingStatus::Phase::Succeeded: {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            statusDot(ImGui::GetWindowDrawList(), ImVec2(start.x + px(6.0f), start.y + titleHeight * 0.5f + px(1.0f)),
                      p.ready, false);
            ImGui::Indent(px(24.0f));
            label(TextStyle::Title, format(t.pairingDoneTitle, {model.pairing.consoleName}));
            ImGui::Unindent(px(24.0f));
            space(8.0f);
            paragraph(TextStyle::Body, t.pairingDoneBody, p.textSecondary);
            space(24.0f);
            alignRight(buttonSize(t.done).x);
            if (button(t.done, ButtonKind::Primary) || cancelPressed()) {
                actions_.dismissPairing();
                closeSheet();
                ImGui::CloseCurrentPopup();
            }
            break;
        }
        case PairingStatus::Phase::Failed: {
            label(TextStyle::Title, t.pairingFailedTitle);
            space(8.0f);
            paragraph(TextStyle::Body, pairingFailureText(t, model.pairing.error), p.text);
            if (!model.pairing.errorDetail.empty()) {
                space(10.0f);
                paragraph(TextStyle::Caption, format(t.technicalDetail, {model.pairing.errorDetail}), p.textTertiary);
            }
            space(24.0f);
            const bool cancel = edgeButton(t.cancel);
            ImGui::SameLine();
            alignRight(buttonSize(t.retry).x);
            const bool retry = button(t.retry, ButtonKind::Primary);
            if (cancel) {
                actions_.dismissPairing();
                closeSheet();
                ImGui::CloseCurrentPopup();
            } else if (retry) {
                // A PIN is single-use, so the retry starts with an empty field.
                actions_.dismissPairing();
                pairingPin_.clear();
                pairingStep_ = PairingStep::Pin;
            }
            break;
        }
        case PairingStatus::Phase::Idle: break;
    }
}

// ---- connection check ----------------------------------------------------

void UiManager::drawConnection(const UiModel& model) {
    const Strings& t = *text_;
    const Palette& p = palette();
    if (!beginSheet("connection-sheet", 560.0f)) return;

    const ConnectionTest& test = model.connectionTest;
    const protocol::LatencySnapshot& stats = test.stats;
    using protocol::LinkQuality;

    label(TextStyle::Caption, t.checkConnection, p.textSecondary);
    space(6.0f);
    label(TextStyle::Title, format(t.connectionTitle, {test.consoleName}));
    space(16.0f);

    // The verdict first, in one word, then what it means for playing.
    const float displayHeight = measure(TextStyle::Display, "X").y;
    const ImVec2 verdictStart = ImGui::GetCursorScreenPos();
    if (test.quality == LinkQuality::Unknown) {
        ImGui::SetCursorScreenPos(ImVec2(verdictStart.x, verdictStart.y + (displayHeight - px(22.0f)) * 0.5f + px(1.0f)));
        spinner(11.0f);
        ImGui::SetCursorScreenPos(ImVec2(verdictStart.x + px(34.0f), verdictStart.y));
        label(TextStyle::Display, t.connectionMeasuring);
        space(6.0f);
        paragraph(TextStyle::Body, t.connectionMeasuringHint, p.textSecondary);
    } else {
        const char* word = test.quality == LinkQuality::Excellent ? t.qualityExcellent
                           : test.quality == LinkQuality::Good    ? t.qualityGood
                           : test.quality == LinkQuality::Fair    ? t.qualityFair
                                                                  : t.qualityPoor;
        const char* meaning = test.quality == LinkQuality::Excellent ? t.qualityExcellentHint
                              : test.quality == LinkQuality::Good    ? t.qualityGoodHint
                              : test.quality == LinkQuality::Fair    ? t.qualityFairHint
                                                                     : t.qualityPoorHint;
        statusDot(ImGui::GetWindowDrawList(),
                  ImVec2(verdictStart.x + px(6.0f), verdictStart.y + displayHeight * 0.5f + px(1.0f)),
                  qualityColor(test.quality), false);
        ImGui::Indent(px(24.0f));
        label(TextStyle::Display, word);
        ImGui::Unindent(px(24.0f));
        space(6.0f);
        paragraph(TextStyle::Body, stats.answered() == 0 ? t.connectionNoAnswer : meaning, p.textSecondary);
    }
    space(18.0f);

    // One bar per probe. Even bars are what a steady link looks like.
    {
        const float width = ImGui::GetContentRegionAvail().x;
        const float height = px(64.0f);
        const ImVec2 min = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(min, ImVec2(min.x + width, min.y + height), toU32(p.control, 0.5f), px(8.0f));

        constexpr int kBars = 60;
        const float gap = px(2.0f);
        const float inset = px(8.0f);
        const float barWidth = (width - 2.0f * inset - gap * (kBars - 1)) / kBars;
        const float usable = height - 2.0f * inset;
        const float scale = std::max(20.0f, stats.maximum * 1.15f); // ms at full height
        const int count = static_cast<int>(stats.history.size());
        const int first = std::max(0, count - kBars);
        for (int i = first; i < count; ++i) {
            const float x = min.x + inset + static_cast<float>(i - first) * (barWidth + gap);
            const float value = stats.history[static_cast<std::size_t>(i)];
            const float bottom = min.y + height - inset;
            if (value < 0.0f) {
                draw->AddRectFilled(ImVec2(x, bottom - px(5.0f)), ImVec2(x + barWidth, bottom), toU32(p.danger), px(1.0f));
            } else {
                const float h = std::max(px(2.0f), usable * std::min(1.0f, value / scale));
                draw->AddRectFilled(ImVec2(x, bottom - h), ImVec2(x + barWidth, bottom), toU32(p.accentHover), px(1.0f));
            }
        }
        ImGui::Dummy(ImVec2(width, height));
    }
    space(8.0f);
    paragraph(TextStyle::Caption, t.connectionGraphHint, p.textTertiary);
    space(16.0f);

    // Three figures side by side.
    {
        const bool any = stats.answered() > 0;
        const std::string response = any ? format(t.msValue, {number(stats.average, stats.average < 10.0f ? 1 : 0)}) : "-";
        const std::string jitter = any ? format(t.msValue, {number(stats.jitter, stats.jitter < 10.0f ? 1 : 0)}) : "-";
        const std::string loss = stats.sent > 0 ? format(t.percentValue, {number(stats.lossPercent(), 1)}) : "-";
        const std::pair<const char*, const std::string*> figures[] = {
            {t.statResponse, &response}, {t.statJitter, &jitter}, {t.statLoss, &loss}};

        const float column = ImGui::GetContentRegionAvail().x / 3.0f;
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float captionHeight = measure(TextStyle::Caption, "X").y;
        const float valueHeight = measure(TextStyle::Title, "X").y;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (int i = 0; i < 3; ++i) {
            const float x = start.x + column * static_cast<float>(i);
            drawText(draw, ImVec2(x, start.y), TextStyle::Caption, p.textSecondary, figures[i].first);
            drawText(draw, ImVec2(x, start.y + captionHeight + px(4.0f)), TextStyle::Title, p.text, *figures[i].second);
        }
        ImGui::Dummy(ImVec2(column * 3.0f, captionHeight + px(4.0f) + valueHeight));
    }

    if (test.quality != LinkQuality::Unknown && stats.answered() > 0) {
        space(18.0f);
        hairline();
        space(14.0f);
        const protocol::StreamAdvice advice = protocol::adviceFor(test.quality);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        label(TextStyle::Strong, t.adviceTitle);
        space(4.0f);
        label(TextStyle::Body,
              format(t.adviceText, {std::to_string(advice.bitrateKbps / 1000), advice.stableBuffer ? t.jitterStable : t.jitterLow}),
              p.textSecondary);
        const ImVec2 after = ImGui::GetCursorScreenPos();
        const char* applyText = adviceApplied_ ? t.adviceApplied : t.adviceApply;
        ImGui::SetCursorScreenPos(ImVec2(start.x + width - buttonSize(applyText).x, start.y + px(2.0f)));
        if (button(applyText, ButtonKind::Secondary, ImVec2(0, 0), !adviceApplied_)) {
            actions_.applyStreamAdvice();
            adviceApplied_ = true;
        }
        ImGui::SetCursorScreenPos(after);
        ImGui::Dummy(ImVec2(0.0f, 0.0f));

        if (test.quality == LinkQuality::Fair || test.quality == LinkQuality::Poor) {
            space(18.0f);
            label(TextStyle::Strong, t.tipsTitle);
            space(8.0f);
            pushText(TextStyle::Body);
            const float lineHeight = ImGui::GetFontSize();
            popText();
            for (const char* tip : {t.tipBridge, t.tipPowerline, t.tipBand, t.tipQuiet}) {
                const ImVec2 bullet = ImGui::GetCursorScreenPos();
                ImGui::GetWindowDrawList()->AddCircleFilled(
                    ImVec2(bullet.x + px(5.0f), bullet.y + lineHeight * 0.5f + px(1.0f)), px(2.0f), toU32(p.textTertiary));
                ImGui::Indent(px(18.0f));
                paragraph(TextStyle::Body, tip, p.text);
                ImGui::Unindent(px(18.0f));
                space(8.0f);
            }
        }
    }

    space(20.0f);
    alignRight(buttonSize(t.done).x);
    if (button(t.done, ButtonKind::Primary) || cancelPressed()) {
        actions_.stopConnectionTest();
        closeSheet();
        ImGui::CloseCurrentPopup();
    }
    endSheet();
}

// ---- in-stream display -----------------------------------------------------

void UiManager::drawStreamAlerts(const UiModel& model) {
    const Strings& t = *text_;
    const Palette& p = palette();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    Animator& animator = context().animator;

    // Every note has its place in the row and fades in and out there, so
    // one that leaves does not make the others jump at once.
    const std::array<std::pair<StreamAlert, const char*>, 6> notes = {{
        {StreamAlert::NoController, t.alertNoController},
        {StreamAlert::ConnectionStalls, t.alertConnection},
        {StreamAlert::PicturesLost, t.alertLost},
        {StreamAlert::DecoderBehind, t.alertDecoder},
        {StreamAlert::FewerPictures, t.alertFewer},
        {StreamAlert::BatteryLow, t.alertBattery},
    }};
    const float padX = px(14.0f);
    const float padY = px(8.0f);
    const float dot = px(8.0f);
    const float gap = px(8.0f);
    float y = viewport->Pos.y + px(16.0f);
    for (const auto& [alert, text] : notes) {
        const bool on = std::find(model.alerts.begin(), model.alerts.end(), alert) != model.alerts.end();
        // No window is open here to take ids from; these only have to differ from each other.
        const ImGuiID id = static_cast<ImGuiID>(0x5A1E7000 + static_cast<int>(alert));
        const float shown = animator.to(id, on ? 1.0f : 0.0f, 0.22f, 0.0f);
        if (shown < 0.02f) continue;
        const ImVec2 size = measure(TextStyle::Body, text);
        const float width = padX * 2.0f + dot + gap + size.x;
        const float height = padY * 2.0f + size.y;
        const ImVec2 min(viewport->Pos.x + (viewport->Size.x - width) * 0.5f, y - (1.0f - shown) * px(6.0f));
        draw->AddRectFilled(min, ImVec2(min.x + width, min.y + height), toU32(p.background, 0.86f * shown), height * 0.5f);
        draw->AddCircleFilled(ImVec2(min.x + padX + dot * 0.5f, min.y + height * 0.5f), dot * 0.5f,
                              toU32(p.standby, shown));
        ImVec4 ink = p.text;
        ink.w *= shown;
        drawText(draw, ImVec2(min.x + padX + dot + gap, min.y + padY), TextStyle::Body, ink, text);
        y += (height + px(8.0f)) * shown;
    }
}

void UiManager::drawUpscalingNote(const UiModel& model) {
    if (!model.config || !model.stream) return;
    const Strings& t = *text_;
    int& level = model.config->graphics.superResolutionLevel;
    const int highest = model.rtxVideoAvailable ? 2 : 1;

    // F8 steps through the ways to enlarge the picture while the game runs,
    // so the difference can be seen where it matters: off, Lanczos, RTX Video.
    if (ImGui::IsKeyPressed(ImGuiKey_F8, false)) {
        level = level >= highest ? 0 : level + 1;
        actions_.configChanged();
        upscalingNoteFor_ = 4.0;
    }
    if (upscalingNoteFor_ <= 0.0) return;
    upscalingNoteFor_ -= static_cast<double>(ImGui::GetIO().DeltaTime);

    // The first line says what was chosen, the second what happens if that is something else.
    const char* chosen = level <= 0 ? t.off : (level >= 2 && model.rtxVideoAvailable ? "RTX Video" : "Lanczos");
    const std::string title = format(t.upscalingNote, {chosen});
    const char* remark = nullptr;
    if (level > 0 && model.stream->scaler.empty()) remark = t.upscalingNotEnlarged;
    else if (level >= 2 && model.stream->scaler == "Lanczos") remark = t.upscalingRtxRefused;

    const Palette& p = palette();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImVec2 titleSize = measure(TextStyle::Body, title.c_str());
    const ImVec2 remarkSize = remark ? measure(TextStyle::Caption, remark) : ImVec2(0.0f, 0.0f);
    const float padX = px(16.0f);
    const float padY = px(10.0f);
    const float width = padX * 2.0f + std::max(titleSize.x, remarkSize.x);
    const float height = padY * 2.0f + titleSize.y + (remark ? px(4.0f) + remarkSize.y : 0.0f);
    const float shown = static_cast<float>(std::clamp(upscalingNoteFor_ / 0.25, 0.0, 1.0));
    const ImVec2 min(viewport->Pos.x + (viewport->Size.x - width) * 0.5f, viewport->Pos.y + viewport->Size.y - height - px(72.0f));
    draw->AddRectFilled(min, ImVec2(min.x + width, min.y + height), toU32(p.background, 0.86f * shown), px(12.0f));
    ImVec4 ink = p.text;
    ink.w *= shown;
    drawText(draw, ImVec2(min.x + (width - titleSize.x) * 0.5f, min.y + padY), TextStyle::Body, ink, title.c_str());
    if (remark) {
        ImVec4 quiet = p.textSecondary;
        quiet.w *= shown;
        drawText(draw, ImVec2(min.x + (width - remarkSize.x) * 0.5f, min.y + padY + titleSize.y + px(4.0f)),
                 TextStyle::Caption, quiet, remark);
    }
}

/// The player's own keys over the running picture, for the moment one of
/// them is forgotten. The console only ever shows controller symbols.
void UiManager::drawKeyOverlay(const UiModel& model) {
    if (!model.config || !model.config->controller.keyboardMouse.enabled) {
        keyOverlay_ = false;
        return;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F1, false)) keyOverlay_ = !keyOverlay_;
    const float shown = context().animator.to(static_cast<ImGuiID>(0x5A1E7200), keyOverlay_ ? 1.0f : 0.0f, 0.14f, 0.0f);
    if (shown < 0.02f) return;

    const Strings& t = *text_;
    const Palette& p = palette();
    const input::KeyboardMouseSettings& all = model.config->controller.keyboardMouse;
    const input::GameKeyLayout* game = input::gameLayout(all, model.currentGameId);
    const std::array<int, input::kPadControlCount>& keys = game ? game->keys : all.keys;
    const std::string title =
        format(t.kbmOverlayTitle, {game ? (game->name.empty() ? game->titleId : game->name) : std::string(t.kbmAllGames)});

    // Only what has a key is listed, in the two columns of the settings.
    struct Row {
        std::string key;
        std::string control;
    };
    const auto rowsOf = [&](std::span<const input::PadControl> controls) {
        std::vector<Row> rows;
        for (const input::PadControl control : controls) {
            const int code = keys[static_cast<std::size_t>(control)];
            if (code != input::key::None) rows.push_back({keyLabel(t, code), controlLabel(t, control)});
        }
        return rows;
    };
    const std::vector<Row> left = rowsOf(kLeftHand);
    const std::vector<Row> right = rowsOf(kRightHand);

    const float pad = px(18.0f);
    const float rowHeight = px(26.0f);
    const float chipWidth = px(104.0f);
    const float columnWidth = px(300.0f);
    const float columnGap = px(24.0f);
    const float titleHeight = measure(TextStyle::Strong, title).y;
    const float noteHeight = measure(TextStyle::Caption, t.kbmOverlayClose).y;
    const float rows = static_cast<float>(std::max(left.size(), right.size()));
    const ImVec2 size(pad * 2.0f + columnWidth * 2.0f + columnGap,
                      pad + titleHeight + px(12.0f) + rows * rowHeight + px(8.0f) + noteHeight + pad);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 min(viewport->Pos.x + std::floor((viewport->Size.x - size.x) * 0.5f),
                     viewport->Pos.y + std::floor((viewport->Size.y - size.y) * 0.5f) + (1.0f - shown) * px(8.0f));
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    draw->AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y), toU32(p.background, 0.88f * shown), px(14.0f));
    drawText(draw, ImVec2(min.x + pad, min.y + pad), TextStyle::Strong, p.text, title, shown);
    const float top = min.y + pad + titleHeight + px(12.0f);
    const auto column = [&](const std::vector<Row>& list, float x) {
        for (std::size_t i = 0; i < list.size(); ++i) {
            const ImVec2 chipMin(x, top + static_cast<float>(i) * rowHeight);
            const ImVec2 chipMax(x + chipWidth, chipMin.y + rowHeight - px(4.0f));
            draw->AddRectFilled(chipMin, chipMax, toU32(p.control, shown), px(6.0f));
            const ImVec2 keySize = measure(TextStyle::Caption, list[i].key);
            draw->PushClipRect(chipMin, chipMax, true);
            drawText(draw, ImVec2(chipMin.x + std::max(px(6.0f), (chipWidth - keySize.x) * 0.5f),
                                  chipMin.y + (chipMax.y - chipMin.y - keySize.y) * 0.5f),
                     TextStyle::Caption, p.text, list[i].key, shown);
            draw->PopClipRect();
            const float textY = chipMin.y + (chipMax.y - chipMin.y - measure(TextStyle::Body, "X").y) * 0.5f;
            drawText(draw, ImVec2(chipMax.x + px(10.0f), textY), TextStyle::Body, p.textSecondary, list[i].control, shown);
        }
    };
    column(left, min.x + pad);
    column(right, min.x + pad + columnWidth + columnGap);
    drawText(draw, ImVec2(min.x + pad, top + rows * rowHeight + px(8.0f)), TextStyle::Caption, p.textTertiary,
             t.kbmOverlayClose, shown);
}

void UiManager::drawStreamOverlay(const UiModel& model) {
    const Strings& t = *text_;
    const Palette& p = palette();
    const StreamStats& stats = *model.stream;

    // F9 shows and hides the display without leaving the game.
    if (model.config && ImGui::IsKeyPressed(ImGuiKey_F9, false)) {
        model.config->network.statsOverlay = !model.config->network.statsOverlay;
        actions_.configChanged();
    }
    if (model.config && !model.config->network.statsOverlay) return;

    std::string frameGeneration = t.off;
    if (stats.frameGenerationFactor > 1) {
        frameGeneration = std::to_string(stats.frameGenerationFactor) + "x";
        if (!stats.frameGenerationEngine.empty()) frameGeneration += "  ·  " + stats.frameGenerationEngine;
        if (!stats.frameGenerationAvailable) frameGeneration += std::string("  ·  ") + t.hudUnavailable;
        else if (!stats.frameGenerationActive) frameGeneration += std::string("  ·  ") + t.hudPaused;
    }
    // Rows about the picture only make sense once a picture is shown.
    std::vector<std::pair<const char*, std::string>> rows;
    // What arrives: the size the decoder found and the rate that was counted, not what was asked for.
    std::string arriving = format(t.fpsValue, {number(stats.streamFps, 1)});
    if (stats.presenting && stats.inputWidth > 0) {
        arriving = std::to_string(stats.inputWidth) + " x " + std::to_string(stats.inputHeight) + "  ·  " + arriving;
    }
    rows.emplace_back(t.hudConsole, arriving);
    // A game that runs slower than the stream: the console sends each of its pictures twice.
    if (stats.presenting && stats.gameFps > 0) {
        rows.emplace_back(t.hudGame, format(t.fpsValue, {std::to_string(stats.gameFps)}));
    }
    if (stats.presenting) {
        std::string shown = format(t.fpsValue, {number(stats.outputFps, 1)});
        if (stats.outputWidth > 0) {
            shown = std::to_string(stats.outputWidth) + " x " + std::to_string(stats.outputHeight) + "  ·  " + shown;
        }
        rows.emplace_back(t.hudDisplay, shown);
        rows.emplace_back(t.hudFrameGeneration, frameGeneration);
        rows.emplace_back(t.hudAddedDelay, format(t.msValue, {number(stats.addedLatencyMs, 0)}));
        if (!stats.scaler.empty()) rows.emplace_back(t.upscaling, stats.scaler);
    }
    rows.emplace_back(t.hudBitrate, format(t.bitrateValue, {number(stats.bitrateMbps, 1)}));
    rows.emplace_back(t.hudResponse, format(t.msValue, {number(stats.roundTripMs, 1)}));
    rows.emplace_back(t.hudLoss, format(t.percentValue, {number(stats.packetLossPercent, 1)}));
    if (stats.presenting && !stats.decoder.empty()) {
        rows.emplace_back(t.hudDecoder, stats.decoder + "  ·  " + format(t.msValue, {number(stats.decodeMs, 1)}));
        // The slowest picture says more about stutter than the average does.
        rows.emplace_back(t.hudDecodeSlowest, format(t.msValue, {number(stats.decodeMaxMs, 1)}));
        // The whole way through this PC: waiting, decoding, waiting for its turn, drawing.
        if (stats.insidePcMs > 0.0f) rows.emplace_back(t.hudInsidePc, format(t.msValue, {number(stats.insidePcMs, 0)}));
    }
    // Only worth a row when it happens: the decoder could not keep up.
    if (stats.presenting && stats.droppedFrames > 0) rows.emplace_back(t.hudDropped, std::to_string(stats.droppedFrames));

    const float pad = px(14.0f);
    const float lineHeight = measure(TextStyle::Caption, "X").y;
    const float rowHeight = lineHeight + px(6.0f);
    float labelWidth = 0.0f, valueWidth = 0.0f;
    for (const auto& [name, value] : rows) {
        labelWidth = std::max(labelWidth, measure(TextStyle::Caption, name).x);
        valueWidth = std::max(valueWidth, measure(TextStyle::Caption, value).x);
    }
    const float inner = std::max(labelWidth + px(20.0f) + valueWidth, px(220.0f));
    const float graphHeight = px(34.0f);
    const float rowsHeight = rowHeight * static_cast<float>(rows.size());
    const float graphBlock = stats.presenting ? px(8.0f) + lineHeight + px(4.0f) + graphHeight : -px(6.0f);
    const ImVec2 size(inner + 2.0f * pad, pad + rowsHeight + graphBlock + pad);

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 min(viewport->Pos.x + px(16.0f), viewport->Pos.y + px(16.0f));
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    // Dark enough to read over a bright picture, light enough not to hide it.
    draw->AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y), toU32(p.background, 0.80f), px(12.0f));

    float y = min.y + pad;
    for (const auto& [name, value] : rows) {
        drawText(draw, ImVec2(min.x + pad, y), TextStyle::Caption, p.textSecondary, name);
        const float width = measure(TextStyle::Caption, value).x;
        drawText(draw, ImVec2(min.x + pad + inner - width, y), TextStyle::Caption, p.text, value);
        y += rowHeight;
    }

    if (!stats.presenting) return;

    // Time between shown frames. A flat line of bars is what smooth looks like.
    y += px(8.0f);
    drawText(draw, ImVec2(min.x + pad, y), TextStyle::Caption, p.textSecondary, t.hudFrameTimes);
    y += lineHeight + px(4.0f);
    draw->AddRectFilled(ImVec2(min.x + pad, y), ImVec2(min.x + pad + inner, y + graphHeight), toU32(p.control, 0.6f), px(6.0f));
    if (!stats.frameTimes.empty()) {
        const float target = stats.outputFps > 1.0f ? 1000.0f / stats.outputFps : 16.7f;
        const float scale = target * 2.0f; // the ideal frame time sits at half height
        const int count = static_cast<int>(stats.frameTimes.size());
        const int bars = std::min(count, 90);
        const float barWidth = inner / 90.0f;
        for (int i = 0; i < bars; ++i) {
            const float value = stats.frameTimes[static_cast<std::size_t>(count - bars + i)];
            const float h = std::max(px(1.0f), graphHeight * std::min(1.0f, value / scale));
            const bool late = value > target * 1.5f;
            const float x = min.x + pad + static_cast<float>(i) * barWidth;
            draw->AddRectFilled(ImVec2(x, y + graphHeight - h), ImVec2(x + std::max(1.0f, barWidth - 1.0f), y + graphHeight),
                                toU32(late ? p.standby : p.accentHover));
        }
    }
}

// ---- session -----------------------------------------------------------

/// The menu over the running picture. Three tiles say how picture, link and
/// controls are doing right now; a click on one leads to its settings. Below
/// them, what is changed most often can be changed on the spot.
void UiManager::drawPauseMenu(const UiModel& model) {
    const Strings& t = *text_;
    const Palette& p = palette();
    const SessionView& session = model.session;
    const StreamStats& stats = *model.stream;
    Animator& animator = context().animator;
    const float width = ImGui::GetContentRegionAvail().x;

    // Who is on the other end, what runs there, and for how long.
    std::string title = session.consoleName;
    for (const ConsoleEntry& console : model.consoles) {
        if (console.hostId == session.hostId && !console.runningApp.empty()) title += "  ·  " + console.runningApp;
    }
    {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float titleHeight = measure(TextStyle::Display, title).y;
        statusDot(ImGui::GetWindowDrawList(), ImVec2(start.x + px(5.0f), start.y + titleHeight * 0.5f + px(1.0f)), p.ready, false);
        ImGui::SetCursorScreenPos(ImVec2(start.x + px(22.0f), start.y));
        label(TextStyle::Display, title);
    }
    space(4.0f);
    char clock[32] = {};
    const int seconds = std::max(0, session.connectedSeconds);
    if (seconds >= 3600) std::snprintf(clock, sizeof(clock), "%d:%02d:%02d", seconds / 3600, seconds / 60 % 60, seconds % 60);
    else std::snprintf(clock, sizeof(clock), "%d:%02d", seconds / 60, seconds % 60);
    label(TextStyle::Caption, format(t.pauseSince, {clock}) + "  ·  " + t.pauseBody, p.textSecondary);
    space(20.0f);

    // ---- the three tiles
    struct Tile {
        const char* name;
        std::string value;
        std::string detail;
        ImVec4 dot;
        SettingsTab tab;
    };
    const bool rough = stats.packetLossPercent > 0.5f ||
                       std::any_of(model.alerts.begin(), model.alerts.end(), [](StreamAlert alert) {
                           return alert == StreamAlert::ConnectionStalls || alert == StreamAlert::PicturesLost;
                       });
    std::string pictureDetail;
    if (stats.inputWidth > 0) pictureDetail = std::to_string(stats.inputWidth) + " x " + std::to_string(stats.inputHeight);
    if (stats.frameGenerationFactor > 1 && stats.frameGenerationAvailable) {
        pictureDetail += std::string(pictureDetail.empty() ? "" : "  ·  ") + t.hudFrameGeneration + " " +
                         std::to_string(stats.frameGenerationFactor) + "x";
    }
    const bool keys = model.config && model.config->controller.keyboardMouse.enabled;
    std::string controlValue = model.controllerConnected ? model.controller.name : keys ? t.kbmTitle : t.pauseNoController;
    std::string controlDetail;
    if (model.controllerConnected) {
        if (model.controller.connection == input::ConnectionType::Usb) controlDetail = t.viaUsb;
        if (model.controller.connection == input::ConnectionType::Bluetooth) controlDetail = t.viaBluetooth;
        if (const std::string battery = batteryText(t, model.controller); !battery.empty()) {
            controlDetail += (controlDetail.empty() ? "" : "  ·  ") + battery;
        }
        if (keys) controlDetail += std::string(controlDetail.empty() ? "" : "  ·  ") + t.kbmTitle;
    }
    const bool lowBattery = model.controllerConnected && model.controller.batteryPercent >= 0 &&
                            model.controller.batteryPercent <= 15 && !model.controller.charging;
    const std::array<Tile, 3> tiles = {{
        {t.pauseTilePicture, format(t.pausePictureValue, {number(stats.outputFps, 0)}), pictureDetail, p.ready,
         SettingsTab::Picture},
        {t.pauseTileLink, rough ? t.pauseLinkRough : t.pauseLinkGood,
         format(t.pauseLinkDetail, {number(stats.bitrateMbps, 1), number(stats.roundTripMs, 0), number(stats.packetLossPercent, 1)}),
         rough ? p.standby : p.ready, SettingsTab::Stream},
        {t.pauseTileControl, controlValue, controlDetail,
         !model.controllerConnected && !keys ? p.standby : lowBattery ? p.standby : p.ready, SettingsTab::Controller},
    }};
    const float gap = px(12.0f);
    const float tileWidth = std::floor((width - 2.0f * gap) / 3.0f);
    const float tileHeight = px(96.0f);
    const float pad = px(14.0f);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (std::size_t i = 0; i < tiles.size(); ++i) {
        const Tile& tile = tiles[i];
        const ImVec2 min(origin.x + static_cast<float>(i) * (tileWidth + gap), origin.y);
        const ImVec2 max(min.x + tileWidth, min.y + tileHeight);
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(static_cast<int>(i));
        const bool clicked = ImGui::InvisibleButton("tile", ImVec2(tileWidth, tileHeight));
        const bool hovered = ImGui::IsItemHovered();
        const bool held = ImGui::IsItemActive();
        const float lit = animator.to(ImGui::GetID("lit"), held ? 1.0f : hovered ? 0.6f : 0.0f, 0.12f, 0.0f);
        ImGui::PopID();
        if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        draw->AddRectFilled(min, max, toU32(mix(p.control, p.controlHover, lit)), px(12.0f));
        draw->AddCircleFilled(ImVec2(min.x + pad + px(4.0f), min.y + pad + px(7.0f)), px(4.0f), toU32(tile.dot));
        drawText(draw, ImVec2(min.x + pad + px(16.0f), min.y + pad), TextStyle::Caption, p.textSecondary, tile.name);
        // Long names (a controller's, say) end at the tile's edge instead of running into the next one.
        draw->PushClipRect(ImVec2(min.x + pad, min.y), ImVec2(max.x - pad, max.y), true);
        drawText(draw, ImVec2(min.x + pad, min.y + pad + px(24.0f)), TextStyle::Strong, p.text, tile.value);
        drawText(draw, ImVec2(min.x + pad, min.y + pad + px(48.0f)), TextStyle::Caption, p.textSecondary, tile.detail);
        draw->PopClipRect();
        if (clicked) showSettingsInGame(tile.tab);
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + tileHeight));
    space(24.0f);

    // ---- what is changed most often
    if (model.config) {
        AppConfig& config = *model.config;
        const AppConfig before = config;
        label(TextStyle::Caption, t.pauseQuick, p.textTertiary);
        space(10.0f);
        const float half = std::floor((width + gap) * 0.5f);
        const float controlWidth = half - gap;
        ImGui::BeginGroup();
        label(TextStyle::Strong, t.frameGeneration);
        space(8.0f);
        const std::array<const char*, 5> factors = {t.automatic2, t.off, "2x", "3x", "4x"};
        config.graphics.frameGenerationFactor =
            segmented("pause-fg", factors, config.graphics.frameGenerationFactor, controlWidth);
        ImGui::EndGroup();
        ImGui::SameLine(half);
        ImGui::BeginGroup();
        label(TextStyle::Strong, t.upscaling);
        space(8.0f);
        if (model.rtxVideoAvailable) {
            const std::array<const char*, 3> methods = {t.off, "Lanczos", "RTX Video"};
            config.graphics.superResolutionLevel =
                segmented("pause-upscaling", methods, config.graphics.superResolutionLevel, controlWidth);
        } else {
            const std::array<const char*, 2> methods = {t.off, "Lanczos"};
            config.graphics.superResolutionLevel =
                segmented("pause-upscaling", methods, config.graphics.superResolutionLevel > 0 ? 1 : 0, controlWidth);
        }
        ImGui::EndGroup();
        space(16.0f);

        const ImVec2 row = ImGui::GetCursorScreenPos();
        ImGui::BeginChild("pause-left", ImVec2(controlWidth, px(32.0f)), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        toggleRow(t.pauseOverlay, &config.network.statsOverlay);
        ImGui::EndChild();
        ImGui::SetCursorScreenPos(ImVec2(row.x + half, row.y));
        ImGui::BeginChild("pause-right", ImVec2(controlWidth, px(32.0f)), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
        toggleRow(t.kbmTitle, &config.controller.keyboardMouse.enabled);
        ImGui::EndChild();
        if (!(config == before)) actions_.configChanged();
        space(24.0f);
    }

    // ---- what to do next. Leaving sits apart on the right, away from the button pressed most.
    const ImVec2 buttons = ImGui::GetCursorScreenPos();
    if (button(t.resume, ButtonKind::Primary)) pauseMenu_ = false;
    ImGui::SameLine(0.0f, px(8.0f));
    if (button(t.settings, ButtonKind::Secondary)) showSettingsInGame(SettingsTab::Picture);
    if (keys) {
        ImGui::SameLine(0.0f, px(8.0f));
        if (button(t.pauseLayout, ButtonKind::Secondary)) {
            showSettingsInGame(SettingsTab::Controller);
            scrollSettingsTo(1.0f);
        }
    }
    const float sleepWidth = buttonSize(t.sleepConsole).x;
    const float leaveWidth = buttonSize(t.disconnect).x;
    ImGui::SetCursorScreenPos(ImVec2(buttons.x + width - sleepWidth - px(8.0f) - leaveWidth, buttons.y));
    if (button(t.disconnect, ButtonKind::Secondary)) actions_.disconnect();
    ImGui::SameLine(0.0f, px(8.0f));
    if (button(t.sleepConsole, ButtonKind::Quiet)) actions_.sleepAndDisconnect();
    space(16.0f);
    label(TextStyle::Caption, t.pauseKeys, p.textTertiary);
}

void UiManager::drawSession(const UiModel& model) {
    const Strings& t = *text_;
    const Palette& p = palette();
    const SessionView& session = model.session;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const bool presenting = model.stream && model.stream->presenting;
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    // The picture stays visible behind the menu, which fades in over it.
    const float shown = presenting && model.state == AppState::Streaming ? menuShown_ : 1.0f;
    if (presenting) ImGui::SetNextWindowBgAlpha(0.80f * shown);
    ImGui::Begin("session", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

    // One short column, a little above the middle: the place the eye rests.
    const float margin = px(kPageMargin);
    // The menu over the picture has more to show than the waiting screens and gets more room.
    const bool menu = presenting && model.state == AppState::Streaming;
    const float column = std::min(px(menu ? 760.0f : 520.0f), viewport->Size.x - 2.0f * margin);
    const float top = menu ? std::max(margin, std::floor((viewport->Size.y - px(470.0f)) * 0.42f))
                           : std::floor(viewport->Size.y * 0.30f);
    ImGui::SetCursorPos(ImVec2(std::floor((viewport->Size.x - column) * 0.5f), top + std::floor((1.0f - shown) * px(10.0f))));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * std::max(shown, 0.02f));
    ImGui::BeginChild("session-column", ImVec2(column, 0.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar);
    pushExplicitSpacing();

    // A title with a spinner or a status dot in front of it.
    const auto heading = [&](const std::string& text, bool busy) {
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float titleHeight = measure(TextStyle::Display, text).y;
        if (busy) {
            ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + (titleHeight - px(22.0f)) * 0.5f + px(1.0f)));
            spinner(11.0f);
            ImGui::SetCursorScreenPos(ImVec2(start.x + px(34.0f), start.y));
        } else {
            statusDot(ImGui::GetWindowDrawList(), ImVec2(start.x + px(5.0f), start.y + titleHeight * 0.5f + px(1.0f)),
                      p.ready, false);
            ImGui::SetCursorScreenPos(ImVec2(start.x + px(22.0f), start.y));
        }
        label(TextStyle::Display, text);
    };
    const std::string attempt = std::to_string(session.attempt);
    const std::string attempts = std::to_string(session.attempts);

    if (model.state == AppState::Connecting && session.loginPinRequested) {
        label(TextStyle::Display, format(t.loginPinTitle, {session.consoleName}));
        space(8.0f);
        paragraph(TextStyle::Body, t.loginPinBody, p.textSecondary);
        space(16.0f);
        const bool complete = loginPin_.size() >= 4;
        const bool enter = textField(t.loginPinLabel, loginPin_, "0000",
                                     session.loginPinWrong && loginPin_.empty() ? FieldState::Invalid : FieldState::Neutral,
                                     session.loginPinWrong && loginPin_.empty() ? t.loginPinWrong : "",
                                     ImGuiInputTextFlags_CharsDecimal | ImGuiInputTextFlags_Password |
                                         ImGuiInputTextFlags_EnterReturnsTrue,
                                     px(200.0f), TextStyle::Title);
        space(20.0f);
        const bool submit = button(t.loginPinSubmit, ButtonKind::Primary, ImVec2(0, 0), complete);
        ImGui::SameLine();
        if (button(t.cancel, ButtonKind::Quiet) || cancelPressed()) actions_.disconnect();
        if ((submit || enter) && complete) {
            actions_.submitLoginPin(loginPin_);
            loginPin_.clear();
        }
    } else if (model.state == AppState::Connecting) {
        heading(format(t.connecting, {session.consoleName}), true);
        space(8.0f);
        paragraph(TextStyle::Body,
                  session.waitingForRelease ? std::string(t.waitingForRelease)
                  : session.attempt > 1     ? format(t.connectingAttempt, {attempt, attempts})
                                            : std::string(t.connectingHint),
                  p.textSecondary);
        space(24.0f);
        if (button(t.cancel, ButtonKind::Secondary) || cancelPressed()) actions_.disconnect();
    } else if (model.state == AppState::Reconnecting) {
        heading(format(t.reconnectingTitle, {session.consoleName}), true);
        space(8.0f);
        paragraph(TextStyle::Body,
                  session.waitingForRelease ? std::string(t.waitingForRelease) : format(t.reconnectingBody, {attempt, attempts}),
                  p.textSecondary);
        space(24.0f);
        if (button(t.cancel, ButtonKind::Secondary) || cancelPressed()) actions_.disconnect();
    } else if (model.state == AppState::Disconnecting) {
        heading(t.disconnecting, true);
    } else if (menu) {
        drawPauseMenu(model);
    } else {
        heading(format(t.streamingTitle, {session.consoleName}), false);
        space(8.0f);
        const char* body = presenting ? t.pauseBody
                           : session.connectedSeconds > 5 ? t.streamingNoPicture
                                                          : t.streamingBody;
        paragraph(TextStyle::Body, body, p.textSecondary);
        if (!model.controllerConnected) {
            space(8.0f);
            paragraph(TextStyle::Body, t.streamingNoController, p.standby);
        }
        space(16.0f);
        label(TextStyle::Caption,
              format(t.streamingReceived, {std::to_string(session.videoFrames), std::to_string(session.audioPackets),
                                           number(static_cast<float>(session.receivedMegabytes), 1)}),
              p.textTertiary);
        if (session.framesLost > 0) {
            space(4.0f);
            label(TextStyle::Caption, format(t.streamingLost, {std::to_string(session.framesLost)}), p.textTertiary);
        }
        space(24.0f);
        // The controller is busy playing, so these are for mouse and keyboard.
        if (presenting) {
            if (button(t.resume, ButtonKind::Primary)) pauseMenu_ = false;
            ImGui::SameLine();
        }
        if (button(t.disconnect, presenting ? ButtonKind::Secondary : ButtonKind::Primary)) actions_.disconnect();
        ImGui::SameLine();
        if (button(t.sleepConsole, ButtonKind::Secondary)) actions_.sleepAndDisconnect();

        if (presenting && model.config) {
            // What can be changed without leaving the game.
            space(24.0f);
            hairline();
            space(12.0f);
            bool overlay = model.config->network.statsOverlay;
            toggleRow(t.pauseOverlay, &overlay);
            if (overlay != model.config->network.statsOverlay) {
                model.config->network.statsOverlay = overlay;
                actions_.configChanged();
            }
            space(12.0f);
            // The gear sits in front of the label; the blanks keep its place free.
            const float blank = measure(TextStyle::Strong, " ").x;
            const float gear = px(16.0f);
            const int blanks = static_cast<int>(std::ceil((gear + px(8.0f)) / std::max(blank, 1.0f)));
            const std::string caption = std::string(static_cast<std::size_t>(blanks), ' ') + t.settings + "##in-game";
            if (button(caption.c_str(), ButtonKind::Secondary)) showSettingsInGame(SettingsTab::Picture);
            const ImVec2 min = ImGui::GetItemRectMin();
            const ImVec2 max = ImGui::GetItemRectMax();
            const float textWidth = measure(TextStyle::Strong, std::string(static_cast<std::size_t>(blanks), ' ') + t.settings).x;
            drawGear(ImGui::GetWindowDrawList(),
                     ImVec2(min.x + ((max.x - min.x) - textWidth) * 0.5f + gear * 0.5f, (min.y + max.y) * 0.5f),
                     gear * 0.5f, toU32(p.text));
        }
    }

    popExplicitSpacing();
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::End();
}

// ---- settings ----------------------------------------------------------

void UiManager::drawSettings(const UiModel& model) {
    const Strings& t = *text_;
    if (!beginSheet("settings-sheet", 600.0f)) return;
    if (!model.config) {
        endSheet();
        return;
    }
    AppConfig& config = *model.config;
    const AppConfig before = config;

    label(TextStyle::Title, t.settings);
    space(14.0f);
    const std::array<const char*, 4> tabs = {t.tabStream, t.tabPicture, t.tabController, t.tabAdvanced};
    settingsTab_ = static_cast<SettingsTab>(
        segmented("tabs", tabs, static_cast<int>(settingsTab_), ImGui::GetContentRegionAvail().x));
    space(18.0f);

    // A fixed height keeps the sheet from jumping when the tab changes. The
    // scrollbar lives in the sheet's right margin, so controls keep their edge.
    const float contentWidth = ImGui::GetContentRegionAvail().x;
    const float bodyHeight = std::min(px(440.0f), ImGui::GetMainViewport()->Size.y - px(260.0f));
    const ImVec2 bodyMin = ImGui::GetCursorScreenPos();
    ImGui::BeginChild("settings-body", ImVec2(contentWidth + px(12.0f), bodyHeight), ImGuiChildFlags_None,
                      ImGuiWindowFlags_NoBackground);
    if (settingsScrollTo_ >= 0.0f) ImGui::SetScrollY(ImGui::GetScrollMaxY() * settingsScrollTo_);
    const float scrollY = ImGui::GetScrollY();
    const float scrollMax = ImGui::GetScrollMaxY();
    ImGui::BeginChild("settings-content", ImVec2(contentWidth, 0.0f), ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoBackground);
    switch (settingsTab_) {
        case SettingsTab::Stream: drawStreamTab(model, config); break;
        case SettingsTab::Picture: drawPictureTab(model, config); break;
        case SettingsTab::Controller: drawControllerTab(model, config); break;
        case SettingsTab::Advanced: drawAdvancedTab(model, config); break;
    }
    space(4.0f);
    ImGui::EndChild();
    ImGui::EndChild();

    // Content that continues beyond the visible part fades out instead of being cut.
    const ImU32 solid = toU32(palette().surfaceRaised);
    const ImU32 clear = toU32(palette().surfaceRaised, 0.0f);
    const float fade = px(24.0f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (scrollY > 1.0f) {
        draw->AddRectFilledMultiColor(bodyMin, ImVec2(bodyMin.x + contentWidth, bodyMin.y + fade), solid, solid, clear, clear);
    }
    if (scrollY < scrollMax - 1.0f) {
        draw->AddRectFilledMultiColor(ImVec2(bodyMin.x, bodyMin.y + bodyHeight - fade),
                                      ImVec2(bodyMin.x + contentWidth, bodyMin.y + bodyHeight), clear, clear, solid, solid);
    }

    space(18.0f);
    alignRight(buttonSize(t.done).x);
    const bool done = button(t.done, ButtonKind::Primary) || cancelPressed();

    // Changes apply as they are made; there is nothing to confirm or discard.
    if (!(config == before)) actions_.configChanged();
    if (done) {
        closeSheet();
        ImGui::CloseCurrentPopup();
    }
    endSheet();
}

void UiManager::drawStreamTab(const UiModel& model, AppConfig& config) {
    const Strings& t = *text_;
    flowStrip(t, model, config, false);
    space(12.0f);
    paragraph(TextStyle::Caption, t.inputIntro, palette().textSecondary);
    if (model.state == AppState::Streaming && model.session.settingsChanged) {
        // Opened from inside the game: the console keeps sending what it was
        // asked for until the session is started again.
        space(12.0f);
        paragraph(TextStyle::Caption, t.inputApplyHint, palette().standby);
        space(8.0f);
        if (button(t.inputApply, ButtonKind::Secondary)) {
            actions_.applyStreamSettings();
            closeSheet();
            pauseMenu_ = false;
        }
    }
    space(16.0f);

    // One switch instead of five choices. The values below stay stored and
    // come back when it is switched off.
    bool automatic = isAutomatic(config);
    if (toggleRow(t.inputAuto, &automatic)) {
        // Switched off, sanitize() names the profile the stored values match.
        config.activeProfile = std::string(automatic ? kProfileAuto : kProfileCustom);
    }
    paragraph(TextStyle::Caption, t.inputAutoHint, palette().textSecondary);
    if (automatic) {
        space(8.0f);
        paragraph(TextStyle::Caption, t.inputAutoHdr, palette().textSecondary);
        return;
    }
    space(18.0f);

    // Frame rate and resolution side by side: two short choices of the same kind.
    const float half = ImGui::GetContentRegionAvail().x * 0.5f;
    ImGui::BeginGroup();
    sectionTitle(t.frameRate, true);
    const std::array<const char*, 2> rates = {"30 fps", "60 fps"};
    config.stream.fps = segmented("fps", rates, config.stream.fps >= 60 ? 1 : 0) == 1 ? 60 : 30;
    ImGui::EndGroup();
    ImGui::SameLine(half);
    ImGui::BeginGroup();
    sectionTitle(t.resolution, true);
    const std::array<const char*, 2> resolutions = {"1080p", "720p"};
    const bool hd = segmented("resolution", resolutions, config.stream.height == 720 ? 1 : 0) == 1;
    config.stream.width = hd ? 1280 : 1920;
    config.stream.height = hd ? 720 : 1080;
    ImGui::EndGroup();

    sectionTitle(t.codec);
    const std::array<const char*, 2> codecs = {"HEVC", "H.264"};
    config.stream.codec = segmented("codec", codecs, config.stream.codec == VideoCodec::H264 ? 1 : 0) == 1
                              ? VideoCodec::H264
                              : VideoCodec::Hevc;
    hint(t.codecHint);

    space(14.0f);
    toggleRow(t.hdr, &config.stream.hdr, config.stream.codec == VideoCodec::Hevc);
    paragraph(TextStyle::Caption, t.hdrHint, palette().textSecondary);

    space(18.0f);
    float mbit = static_cast<float>(config.stream.bitrateKbps) / 1000.0f;
    const std::string value = format(t.bitrateValue, {std::to_string(static_cast<int>(std::lround(mbit)))});
    if (sliderRow(t.bitrate, &mbit, kMinBitrateKbps / 1000.0f, kMaxBitrateKbps / 1000.0f, value)) {
        config.stream.bitrateKbps = static_cast<int>(std::lround(mbit)) * 1000;
    }
    hint(t.bitrateHint);
}

void UiManager::drawPictureTab(const UiModel& model, AppConfig& config) {
    const Strings& t = *text_;
    flowStrip(t, model, config, true);
    space(12.0f);
    paragraph(TextStyle::Caption, t.outputIntro, palette().textSecondary);
    space(16.0f);

    sectionTitle(t.frameGeneration, true);
    // The position on the control is the stored value: 0 is automatic, 1 is off.
    const std::array<const char*, 5> factors = {t.automatic2, t.off, "2x", "3x", "4x"};
    config.graphics.frameGenerationFactor = segmented("fg", factors, config.graphics.frameGenerationFactor);
    const int streamFps = isAutomatic(config) ? model.requestedStream.fps : config.stream.fps;
    const std::string fps = std::to_string(streamFps);
    const bool automatic = config.graphics.frameGenerationFactor == 0;
    const int factor = media::frameGenerationFactorFor(config.graphics.frameGenerationFactor, streamFps, model.displayHz);
    // The real picture is shown this long after it arrived, so the ones before it fit in.
    const std::string delay = std::to_string(static_cast<int>(std::lround(1000.0 / streamFps * (factor - 1) / factor)));
    const std::string shown = std::to_string(streamFps * factor);
    if (!automatic && factor < config.graphics.frameGenerationFactor) {
        // The screen has fewer refreshes than the chosen factor needs.
        hint(format(t.frameGenerationCapped,
                    {std::to_string(static_cast<int>(std::lround(model.displayHz))), std::to_string(factor), fps}));
    } else if (factor > 1) {
        hint(format(automatic ? t.frameGenerationAutoOn : t.frameGenerationHint, {fps, shown, delay}));
    } else {
        hint(format(automatic ? t.frameGenerationAutoOff : t.frameGenerationOffHint, {fps}));
    }

    space(18.0f);
    if (model.rtxVideoAvailable) {
        // An NVIDIA card: its network is a third way to enlarge the picture.
        sectionTitle(t.upscaling, true);
        const std::array<const char*, 3> methods = {t.off, "Lanczos", "RTX Video"};
        config.graphics.superResolutionLevel = segmented("upscaling", methods, config.graphics.superResolutionLevel);
        hint(config.graphics.superResolutionLevel >= 2 ? t.upscalingRtxHint : t.upscalingHint);
    } else {
        bool upscale = config.graphics.superResolutionLevel > 0;
        toggleRow(t.upscaling, &upscale);
        // A file from a PC with an NVIDIA card keeps its choice; here it means Lanczos.
        if (upscale != (config.graphics.superResolutionLevel > 0)) config.graphics.superResolutionLevel = upscale ? 1 : 0;
        paragraph(TextStyle::Caption, t.upscalingHint, palette().textSecondary);
    }

    space(18.0f);
    float sharpness = config.graphics.sharpness * 100.0f;
    if (sliderRow(t.sharpness, &sharpness, 0.0f, 100.0f, std::to_string(static_cast<int>(std::lround(sharpness))) + " %")) {
        config.graphics.sharpness = std::round(sharpness) / 100.0f;
    }
    hint(t.sharpnessHint);

    space(14.0f);
    toggleRow(t.vsync, &config.graphics.vsync);
    paragraph(TextStyle::Caption, t.vsyncHint, palette().textSecondary);
}

void UiManager::drawControllerTab(const UiModel& model, AppConfig& config) {
    const Strings& t = *text_;

    sectionTitle(t.activeController, true);
    const std::string current = config.controller.preferredName.empty() ? t.automatic : config.controller.preferredName;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8.0f), px(8.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(8.0f), px(6.0f)));
    if (ImGui::BeginCombo("##controller", current.c_str())) {
        if (ImGui::Selectable(t.automatic, config.controller.preferredName.empty())) config.controller.preferredName.clear();
        for (const input::ControllerInfo& info : model.controllers) {
            if (ImGui::Selectable(info.name.c_str(), info.name == config.controller.preferredName)) {
                config.controller.preferredName = info.name;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::PopStyleVar(2);
    hint(model.controllerConnected ? t.controllerAutoHint : t.controllerNone);

    space(20.0f);
    float deadzone = config.controller.deadzone * 100.0f;
    if (sliderRow(t.deadzone, &deadzone, 0.0f, 30.0f, std::to_string(static_cast<int>(std::lround(deadzone))) + " %")) {
        config.controller.deadzone = std::round(deadzone) / 100.0f;
    }
    hint(t.deadzoneHint);

    space(14.0f);
    toggleRow(t.rumble, &config.controller.rumble);
    space(2.0f);
    toggleRow(t.adaptiveTriggers, &config.controller.adaptiveTriggers);
    hint(t.adaptiveTriggersHint);

    drawKeyboardMouse(model, config);
}

namespace {

/// What a key is called on its cap.
std::string keyLabel(const Strings& t, int code) {
    namespace key = input::key;
    if ((code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9')) return std::string(1, static_cast<char>(code));
    if (code >= key::F1 && code <= key::F1 + 23) return "F" + std::to_string(code - key::F1 + 1);
    if (code >= 0x60 && code <= 0x69) return "Num " + std::to_string(code - 0x60);
    switch (code) {
        case key::None: return t.kbmUnassigned;
        case key::MouseLeft: return t.keyMouseLeft;
        case key::MouseRight: return t.keyMouseRight;
        case key::MouseMiddle: return t.keyMouseMiddle;
        case key::MouseBack: return format(t.keyMouseSide, {"4"});
        case key::MouseForward: return format(t.keyMouseSide, {"5"});
        case key::WheelUp: return t.keyWheelUp;
        case key::WheelDown: return t.keyWheelDown;
        case key::Space: return t.keySpace;
        case key::Shift: return t.keyShift;
        case key::Ctrl: return t.keyCtrl;
        case key::Alt: return "Alt";
        case key::Tab: return "Tab";
        case key::Enter: return t.keyEnter;
        case key::Backspace: return t.keyBackspace;
        case key::Up: return format(t.keyArrow, {t.padUp});
        case key::Down: return format(t.keyArrow, {t.padDown});
        case key::Left: return format(t.keyArrow, {t.padLeft});
        case key::Right: return format(t.keyArrow, {t.padRight});
        case key::Home: return "Pos1 / Home";
        case 0x23: return "Ende / End";
        case 0x21: return "Bild auf / PgUp";
        case 0x22: return "Bild ab / PgDn";
        case 0x2D: return "Einfg / Ins";
        case 0x14: return "Caps Lock";
        default: return format(t.keyOther, {std::to_string(code)});
    }
}

std::string controlLabel(const Strings& t, input::PadControl control) {
    using input::PadControl;
    switch (control) {
        case PadControl::Cross: return t.padCross;
        case PadControl::Circle: return t.padCircle;
        case PadControl::Square: return t.padSquare;
        case PadControl::Triangle: return t.padTriangle;
        case PadControl::DpadUp: return format(t.padDpad, {t.padUp});
        case PadControl::DpadDown: return format(t.padDpad, {t.padDown});
        case PadControl::DpadLeft: return format(t.padDpad, {t.padLeft});
        case PadControl::DpadRight: return format(t.padDpad, {t.padRight});
        case PadControl::L1: return "L1";
        case PadControl::R1: return "R1";
        case PadControl::L2: return "L2";
        case PadControl::R2: return "R2";
        case PadControl::L3: return "L3";
        case PadControl::R3: return "R3";
        case PadControl::Options: return "Options";
        case PadControl::Create: return "Create";
        case PadControl::Touchpad: return t.padTouchpad;
        case PadControl::Ps: return t.padPs;
        case PadControl::LeftStickUp: return format(t.padLeftStick, {t.padUp});
        case PadControl::LeftStickDown: return format(t.padLeftStick, {t.padDown});
        case PadControl::LeftStickLeft: return format(t.padLeftStick, {t.padLeft});
        case PadControl::LeftStickRight: return format(t.padLeftStick, {t.padRight});
        case PadControl::RightStickUp: return format(t.padRightStick, {t.padUp});
        case PadControl::RightStickDown: return format(t.padRightStick, {t.padDown});
        case PadControl::RightStickLeft: return format(t.padRightStick, {t.padLeft});
        case PadControl::RightStickRight: return format(t.padRightStick, {t.padRight});
        case PadControl::Count: break;
    }
    return {};
}

/// A controller, drawn from above, with one of its parts lit. `lit` is how
/// strongly, per control; the shapes follow the DualSense loosely.
void drawGamepad(ImDrawList* draw, ImVec2 origin, float scale, const std::array<float, input::kPadControlCount>& lit) {
    using input::PadControl;
    const Palette& p = palette();
    const auto at = [&](float x, float y) { return ImVec2(origin.x + x * scale, origin.y + y * scale); };
    const auto glow = [&](PadControl control) { return lit[static_cast<std::size_t>(control)]; };
    const auto mix = [&](const ImVec4& base, float amount) {
        return toU32(ImVec4(base.x + (p.accent.x - base.x) * amount, base.y + (p.accent.y - base.y) * amount,
                            base.z + (p.accent.z - base.z) * amount, 1.0f));
    };
    const ImU32 body = toU32(p.control);
    const ImVec4 part = p.controlHover;
    const ImVec4 ink = p.textTertiary;

    // Triggers and shoulder buttons sit behind the body.
    draw->AddRectFilled(at(70, 6), at(128, 40), mix(part, glow(PadControl::L2)), 10.0f * scale);
    draw->AddRectFilled(at(292, 6), at(350, 40), mix(part, glow(PadControl::R2)), 10.0f * scale);
    draw->AddRectFilled(at(56, 30), at(140, 52), mix(part, glow(PadControl::L1)), 8.0f * scale);
    draw->AddRectFilled(at(280, 30), at(364, 52), mix(part, glow(PadControl::R1)), 8.0f * scale);
    // The body with its two grips.
    draw->AddRectFilled(at(40, 44), at(380, 170), body, 44.0f * scale);
    draw->AddCircleFilled(at(82, 172), 46.0f * scale, body, 48);
    draw->AddCircleFilled(at(338, 172), 46.0f * scale, body, 48);

    draw->AddRectFilled(at(152, 50), at(268, 104), mix(part, glow(PadControl::Touchpad)), 10.0f * scale);
    draw->AddRectFilled(at(132, 58), at(144, 76), mix(part, glow(PadControl::Create)), 5.0f * scale);
    draw->AddRectFilled(at(276, 58), at(288, 76), mix(part, glow(PadControl::Options)), 5.0f * scale);
    draw->AddCircleFilled(at(210, 136), 9.0f * scale, mix(part, glow(PadControl::Ps)), 24);

    // Directional buttons.
    const auto dpad = [&](PadControl control, float x, float y) {
        draw->AddRectFilled(at(x - 9, y - 9), at(x + 9, y + 9), mix(part, glow(control)), 4.0f * scale);
    };
    dpad(PadControl::DpadUp, 92, 80);
    dpad(PadControl::DpadDown, 92, 118);
    dpad(PadControl::DpadLeft, 73, 99);
    dpad(PadControl::DpadRight, 111, 99);

    // The four buttons, each with its sign.
    const float sign = 5.0f * scale;
    const float line = std::max(1.0f, 1.6f * scale);
    const auto face = [&](PadControl control, float x, float y) {
        draw->AddCircleFilled(at(x, y), 12.0f * scale, mix(part, glow(control)), 24);
        return at(x, y);
    };
    const ImU32 mark = toU32(ink, 1.0f);
    ImVec2 c = face(PadControl::Triangle, 328, 76);
    draw->AddTriangle(ImVec2(c.x, c.y - sign), ImVec2(c.x - sign, c.y + sign * 0.8f), ImVec2(c.x + sign, c.y + sign * 0.8f), mark, line);
    c = face(PadControl::Circle, 351, 99);
    draw->AddCircle(c, sign, mark, 20, line);
    c = face(PadControl::Cross, 328, 122);
    draw->AddLine(ImVec2(c.x - sign, c.y - sign), ImVec2(c.x + sign, c.y + sign), mark, line);
    draw->AddLine(ImVec2(c.x - sign, c.y + sign), ImVec2(c.x + sign, c.y - sign), mark, line);
    c = face(PadControl::Square, 305, 99);
    draw->AddRect(ImVec2(c.x - sign, c.y - sign), ImVec2(c.x + sign, c.y + sign), mark, 0.0f, 0, line);

    // The sticks: the ring lights up towards the side that is meant, the cap for a press.
    const auto stick = [&](float x, float y, PadControl up, PadControl press) {
        const ImVec2 centre = at(x, y);
        draw->AddCircleFilled(centre, 24.0f * scale, toU32(part), 32);
        const PadControl sides[4] = {up, static_cast<PadControl>(static_cast<int>(up) + 1),
                                     static_cast<PadControl>(static_cast<int>(up) + 2),
                                     static_cast<PadControl>(static_cast<int>(up) + 3)};
        // Up, down, left, right as quarter arcs of the ring.
        const float middles[4] = {-1.5708f, 1.5708f, 3.1416f, 0.0f};
        for (int i = 0; i < 4; ++i) {
            const float amount = glow(sides[i]);
            if (amount < 0.02f) continue;
            draw->PathArcTo(centre, 21.0f * scale, middles[i] - 0.7f, middles[i] + 0.7f, 16);
            draw->PathStroke(toU32(p.accent, amount), 0, 5.0f * scale);
        }
        draw->AddCircleFilled(centre, 15.0f * scale, mix(p.control, glow(press)), 28);
    };
    stick(150, 142, PadControl::LeftStickUp, PadControl::L3);
    stick(270, 142, PadControl::RightStickUp, PadControl::R3);
}

} // namespace

void UiManager::drawKeyboardMouse(const UiModel& model, AppConfig& config) {
    using input::PadControl;
    const Strings& t = *text_;
    const Palette& p = palette();
    input::KeyboardMouseSettings& settings = config.controller.keyboardMouse;
    Animator& animator = context().animator;

    sectionTitle(t.kbmTitle);
    toggleRow(t.kbmEnable, &settings.enabled);
    paragraph(TextStyle::Caption, t.kbmHint, p.textSecondary);
    if (!settings.enabled) {
        if (bindingControl_ >= 0) {
            bindingControl_ = -1;
            actions_.endKeyCapture();
        }
        return;
    }

    // Which layout the fields below belong to: the one for all games, or a
    // game's own. When the settings open, that is the one in use right now.
    if (!editedGameChosen_) {
        editedGameChosen_ = true;
        editedGameId_ = input::gameLayout(settings, model.currentGameId) ? model.currentGameId : std::string{};
    }
    if (!editedGameId_.empty() && !input::gameLayout(settings, editedGameId_)) editedGameId_.clear();
    const auto stopWaiting = [&] {
        if (bindingControl_ < 0) return;
        bindingControl_ = -1;
        actions_.endKeyCapture();
    };

    space(16.0f);
    label(TextStyle::Strong, t.kbmFor);
    space(8.0f);
    {
        const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
        const float chipHeight = px(30.0f);
        ImVec2 at = ImGui::GetCursorScreenPos();
        const float left = at.x;
        ImDrawList* chips = ImGui::GetWindowDrawList();
        const auto chip = [&](const std::string& name, const std::string& id, int index) {
            const ImVec2 size = measure(TextStyle::Body, name);
            const float chipWidth = size.x + px(28.0f);
            if (at.x + chipWidth > right && at.x > left) {
                at.x = left;
                at.y += chipHeight + px(8.0f);
            }
            ImGui::SetCursorScreenPos(at);
            ImGui::PushID(index);
            const bool clicked = ImGui::InvisibleButton("layout-chip", ImVec2(chipWidth, chipHeight));
            const bool over = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool chosen = editedGameId_ == id;
            const ImVec2 max(at.x + chipWidth, at.y + chipHeight);
            chips->AddRectFilled(at, max, toU32(chosen ? p.accent : over ? p.controlHover : p.control), chipHeight * 0.5f);
            drawText(chips, ImVec2(at.x + px(14.0f), at.y + (chipHeight - size.y) * 0.5f), TextStyle::Body,
                     chosen ? p.onAccent : p.text, name);
            if (clicked && !chosen) {
                editedGameId_ = id;
                stopWaiting();
            }
            at.x += chipWidth + px(8.0f);
        };
        chip(t.kbmAllGames, std::string{}, 0);
        // By index: choosing one must not disturb the list it is drawn from.
        for (std::size_t i = 0; i < settings.games.size(); ++i) {
            const std::string id = settings.games[i].titleId;
            chip(settings.games[i].name.empty() ? id : settings.games[i].name, id, static_cast<int>(i) + 1);
        }
        ImGui::SetCursorScreenPos(ImVec2(left, at.y + chipHeight));
        ImGui::Dummy(ImVec2(1.0f, 1.0f));
    }
    space(8.0f);
    const bool gameKnown = !model.currentGameId.empty();
    const std::string gameName = model.currentGameName.empty() ? model.currentGameId : model.currentGameName;
    if (gameKnown && !input::gameLayout(settings, model.currentGameId) && settings.games.size() < input::kMaxGameLayouts) {
        // A new one starts as a copy of the general layout, which is the nearest thing to what is wanted.
        if (button(format(t.kbmAddGame, {gameName}).c_str(), ButtonKind::Secondary)) {
            input::GameKeyLayout added;
            added.titleId = model.currentGameId;
            added.name = model.currentGameName;
            added.keys = settings.keys;
            added.mouseSensitivity = settings.mouseSensitivity;
            added.invertY = settings.invertY;
            settings.games.push_back(std::move(added));
            editedGameId_ = model.currentGameId;
            stopWaiting();
        }
        space(6.0f);
    } else if (!editedGameId_.empty()) {
        const input::GameKeyLayout* chosen = input::gameLayout(settings, editedGameId_);
        const std::string name = chosen && !chosen->name.empty() ? chosen->name : editedGameId_;
        if (button(format(t.kbmRemoveGame, {name}).c_str(), ButtonKind::Quiet)) {
            std::erase_if(settings.games, [&](const input::GameKeyLayout& game) { return game.titleId == editedGameId_; });
            editedGameId_.clear();
            stopWaiting();
        }
        space(6.0f);
    }
    paragraph(TextStyle::Caption, gameKnown || !settings.games.empty() ? t.kbmGamesHint : t.kbmNoGameHint, p.textSecondary);

    input::GameKeyLayout* edited = input::gameLayout(settings, editedGameId_);
    std::array<int, input::kPadControlCount>& keys = edited ? edited->keys : settings.keys;
    float& mouseSensitivity = edited ? edited->mouseSensitivity : settings.mouseSensitivity;
    bool& invertY = edited ? edited->invertY : settings.invertY;

    space(16.0f);
    float sensitivity = mouseSensitivity * 100.0f;
    if (sliderRow(t.kbmSensitivity, &sensitivity, 20.0f, 400.0f, std::to_string(static_cast<int>(std::lround(sensitivity))) + " %")) {
        mouseSensitivity = std::round(sensitivity / 5.0f) * 5.0f / 100.0f;
    }
    hint(t.kbmSensitivityHint);
    space(8.0f);
    toggleRow(t.kbmInvertY, &invertY);

    sectionTitle(t.kbmLayout);
    paragraph(TextStyle::Caption, t.kbmLayoutHint, p.textSecondary);
    space(12.0f);

    // The key that was waited for has come.
    if (bindingControl_ >= 0 && model.capturedKey != 0) {
        const int code = model.capturedKey;
        if (code == input::key::Delete) {
            keys[static_cast<std::size_t>(bindingControl_)] = input::key::None;
        } else if (code > 0 && !input::isReservedKey(code)) {
            // A key stands for one control: it leaves the one it stood for before.
            for (int& other : keys) {
                if (other == code) other = input::key::None;
            }
            keys[static_cast<std::size_t>(bindingControl_)] = code;
        }
        bindingControl_ = -1;
        actions_.endKeyCapture();
    }


    // The controller itself. Its parts light up for the row under the mouse and for the one that waits.
    const float width = ImGui::GetContentRegionAvail().x;
    const float scale = std::min(1.0f, width / px(520.0f)) * px(1.0f);
    const ImVec2 origin(ImGui::GetCursorScreenPos().x + (width - 420.0f * scale) * 0.5f, ImGui::GetCursorScreenPos().y);
    const ImVec2 padSpace(width, 226.0f * scale);
    const ImVec2 rowsStart(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + padSpace.y + px(8.0f));
    // The drawing can be clicked as well: a part of the controller stands for its row below.
    const bool padClicked = ImGui::InvisibleButton("kbm-pad", ImVec2(padSpace.x, padSpace.y + px(8.0f)));
    int hovered = -1;
    if (ImGui::IsItemHovered()) {
        struct Spot {
            PadControl control;
            float x, y, reach;
        };
        static constexpr Spot kSpots[] = {
            {PadControl::L2, 99, 18, 26},        {PadControl::R2, 321, 18, 26},      {PadControl::L1, 98, 44, 20},
            {PadControl::R1, 322, 44, 20},       {PadControl::Touchpad, 210, 77, 34}, {PadControl::Create, 138, 67, 11},
            {PadControl::Options, 282, 67, 11},  {PadControl::Ps, 210, 136, 12},     {PadControl::DpadUp, 92, 80, 11},
            {PadControl::DpadDown, 92, 118, 11}, {PadControl::DpadLeft, 73, 99, 11}, {PadControl::DpadRight, 111, 99, 11},
            {PadControl::Triangle, 328, 76, 13}, {PadControl::Circle, 351, 99, 13},  {PadControl::Cross, 328, 122, 13},
            {PadControl::Square, 305, 99, 13},
        };
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const float mx = (mouse.x - origin.x) / scale;
        const float my = (mouse.y - origin.y) / scale;
        for (const Spot& spot : kSpots) {
            if (std::hypot(mx - spot.x, my - spot.y) <= spot.reach) hovered = static_cast<int>(spot.control);
        }
        // A stick: the cap is its press, the ring around it the four directions.
        const auto stick = [&](float x, float y, PadControl up, PadControl press) {
            const float dx = mx - x;
            const float dy = my - y;
            const float away = std::hypot(dx, dy);
            if (away > 26.0f) return;
            if (away <= 13.0f) hovered = static_cast<int>(press);
            else if (std::abs(dy) >= std::abs(dx)) hovered = static_cast<int>(up) + (dy < 0.0f ? 0 : 1);
            else hovered = static_cast<int>(up) + (dx < 0.0f ? 2 : 3);
        };
        stick(150, 142, PadControl::LeftStickUp, PadControl::L3);
        stick(270, 142, PadControl::RightStickUp, PadControl::R3);
        if (padClicked && hovered >= 0) {
            bindingControl_ = hovered;
            actions_.captureKey();
        }
    }

    const float columnGap = px(24.0f);
    const float columnWidth = (width - columnGap) * 0.5f;
    const float rowHeight = px(30.0f);
    const float chipWidth = std::min(px(128.0f), columnWidth * 0.5f);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const auto column = [&](std::span<const PadControl> controls, float x) {
        for (std::size_t row = 0; row < controls.size(); ++row) {
            const PadControl control = controls[row];
            const int index = static_cast<int>(control);
            const ImVec2 min(x, rowsStart.y + static_cast<float>(row) * rowHeight);
            ImGui::SetCursorScreenPos(min);
            ImGui::PushID(index);
            const bool clicked = ImGui::InvisibleButton("bind", ImVec2(columnWidth, rowHeight - px(4.0f)));
            const bool over = ImGui::IsItemHovered() || hovered == index;
            ImGui::PopID();
            if (over) hovered = index;
            const bool waiting = bindingControl_ == index;
            if (clicked) {
                if (waiting) {
                    bindingControl_ = -1;
                    actions_.endKeyCapture();
                } else {
                    bindingControl_ = index;
                    actions_.captureKey();
                }
            }
            const float textY = min.y + (rowHeight - px(4.0f) - measure(TextStyle::Body, "X").y) * 0.5f;
            drawText(draw, ImVec2(min.x, textY), TextStyle::Body, over || waiting ? p.text : p.textSecondary,
                     controlLabel(t, control));
            // The key, on something that looks like its cap.
            const ImVec2 chipMin(min.x + columnWidth - chipWidth, min.y);
            const ImVec2 chipMax(min.x + columnWidth, min.y + rowHeight - px(4.0f));
            const float lit = animator.to(ImGui::GetID("kbm-chip") + static_cast<ImGuiID>(index), waiting ? 1.0f : over ? 0.35f : 0.0f, 0.18f, 0.0f);
            draw->AddRectFilled(chipMin, chipMax, toU32(p.control), px(7.0f));
            if (lit > 0.02f) draw->AddRect(chipMin, chipMax, toU32(p.accent, lit), px(7.0f), 0, px(1.5f));
            const int code = keys[static_cast<std::size_t>(index)];
            const std::string text = waiting ? std::string(t.kbmPressKey) : keyLabel(t, code);
            const ImVec4& ink = waiting ? p.accent : code == input::key::None ? p.textTertiary : p.text;
            const ImVec2 size = measure(TextStyle::Caption, text);
            draw->PushClipRect(chipMin, chipMax, true);
            drawText(draw, ImVec2(chipMin.x + std::max(px(6.0f), (chipWidth - size.x) * 0.5f),
                                  chipMin.y + (chipMax.y - chipMin.y - size.y) * 0.5f),
                     TextStyle::Caption, ink, text);
            draw->PopClipRect();
        }
    };
    column(kLeftHand, rowsStart.x);
    column(kRightHand, rowsStart.x + columnWidth + columnGap);
    ImGui::SetCursorScreenPos(ImVec2(rowsStart.x, rowsStart.y + static_cast<float>(kLeftHand.size()) * rowHeight));
    ImGui::Dummy(ImVec2(width, px(1.0f)));

    std::array<float, input::kPadControlCount> lit{};
    for (std::size_t i = 0; i < lit.size(); ++i) {
        const bool on = static_cast<int>(i) == hovered || static_cast<int>(i) == bindingControl_;
        lit[i] = animator.to(ImGui::GetID("kbm-lit") + static_cast<ImGuiID>(i), on ? 1.0f : 0.0f, 0.2f, 0.0f);
    }
    drawGamepad(draw, origin, scale, lit);
    // The right stick is the mouse; that needs no field, only saying.
    {
        const ImVec2 size = measure(TextStyle::Caption, t.kbmMouseLooks);
        drawText(draw, ImVec2(origin.x + 270.0f * scale - size.x * 0.5f, origin.y + 174.0f * scale), TextStyle::Caption,
                 p.textSecondary, t.kbmMouseLooks);
    }

    space(10.0f);
    if (button(t.kbmReset, ButtonKind::Quiet)) {
        keys = input::defaultKeyBindings();
        if (bindingControl_ >= 0) {
            bindingControl_ = -1;
            actions_.endKeyCapture();
        }
    }
}

void UiManager::drawAdvancedTab(const UiModel& model, AppConfig& config) {
    const Strings& t = *text_;
    const Palette& p = palette();

    sectionTitle(t.language, true);
    const std::array<const char*, 3> languages = {t.languageAuto, "Deutsch", "English"};
    const int language = config.language == "de" ? 1 : config.language == "en" ? 2 : 0;
    const int chosenLanguage = segmented("language", languages, language);
    config.language = chosenLanguage == 1 ? "de" : chosenLanguage == 2 ? "en" : "auto";

    if (model.updaterAvailable) {
        sectionTitle(t.updatesTitle);
        using Phase = update::UpdateStatus::Phase;
        const update::UpdateStatus& update = model.update;
        paragraph(TextStyle::Caption, format(t.updatesInstalled, {update.current.toString()}), p.textSecondary);
        space(6.0f);
        toggleRow(t.updatesAuto, &config.updates.checkAtStartup);
        paragraph(TextStyle::Caption, t.updatesAutoHint, p.textSecondary);
        space(10.0f);

        const bool busy = update.phase == Phase::Checking || update.phase == Phase::Downloading ||
                          update.phase == Phase::Installing;
        const ImVec2 start = ImGui::GetCursorScreenPos();
        if (button(t.updatesCheckNow, ButtonKind::Secondary, ImVec2(0, 0), !busy && update.phase != Phase::RestartNeeded)) {
            actions_.checkForUpdates();
        }
        std::string state;
        if (update.phase == Phase::Checking) state = t.updatesChecking;
        else if (update.phase == Phase::UpToDate) state = t.updatesCurrent;
        else if (update.phase == Phase::Available) state = format(t.updateAvailable, {update.latest.toString()});
        else if (update.phase == Phase::Failed && update.error == update::UpdateError::NoRelease) state = t.updatesNone;
        else if (update.phase == Phase::Failed && !update.failedDuringInstall) state = t.updatesOffline;
        if (!state.empty()) {
            const ImVec2 size = measure(TextStyle::Body, state);
            drawText(ImGui::GetWindowDrawList(),
                     ImVec2(start.x + buttonSize(t.updatesCheckNow).x + px(14.0f), start.y + (px(36.0f) - size.y) * 0.5f),
                     TextStyle::Body, p.textSecondary, state);
        }
    }

    sectionTitle(t.jitter);
    const std::array<const char*, 2> jitter = {t.jitterLow, t.jitterStable};
    config.network.jitter = segmented("jitter", jitter, config.network.jitter == JitterMode::Stable ? 1 : 0) == 1
                                ? JitterMode::Stable
                                : JitterMode::LowLatency;
    hint(t.jitterHint);
    space(10.0f);
    toggleRow(t.statsOverlay, &config.network.statsOverlay);

    sectionTitle(t.extraAddresses);
    paragraph(TextStyle::Caption, t.extraAddressesHint, p.textSecondary);
    space(8.0f);
    for (std::size_t i = 0; i < config.manualHosts.size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        const ImVec2 start = ImGui::GetCursorScreenPos();
        const float rowHeight = px(36.0f);
        const ImVec2 size = measure(TextStyle::Body, config.manualHosts[i]);
        drawText(ImGui::GetWindowDrawList(), ImVec2(start.x, start.y + (rowHeight - size.y) * 0.5f), TextStyle::Body,
                 p.text, config.manualHosts[i]);
        alignRight(buttonSize(t.remove).x);
        const bool removed = button(t.remove, ButtonKind::Quiet);
        ImGui::PopID();
        if (removed) {
            config.manualHosts.erase(config.manualHosts.begin() + static_cast<std::ptrdiff_t>(i));
            break;
        }
    }
    if (!config.manualHosts.empty()) space(6.0f);

    const bool addressTyped = !trim(newAddress_).empty();
    const bool addressValid = net::parseIpv4(trim(newAddress_)).has_value();
    const float addWidth = buttonSize(t.add).x;
    const bool enter = textField("##new-address", newAddress_, "192.168.1.20",
                                 addressTyped && !addressValid ? FieldState::Invalid : FieldState::Neutral, "",
                                 ImGuiInputTextFlags_EnterReturnsTrue,
                                 ImGui::GetContentRegionAvail().x - addWidth - px(8.0f));
    ImGui::SameLine();
    if (button(t.add, ButtonKind::Secondary, ImVec2(0, 0), addressValid) || (enter && addressValid)) {
        config.manualHosts.emplace_back(trim(newAddress_));
        newAddress_.clear();
    }

    sectionTitle(t.remoteTitle);
    paragraph(TextStyle::Caption, t.remoteHint, p.textSecondary);
    space(6.0f);
    toggleRow(t.remoteEnable, &config.remoteApi.enabled);
    if (config.remoteApi.enabled) {
        space(8.0f);
        const float half = ImGui::GetContentRegionAvail().x * 0.5f;
        ImGui::BeginGroup();
        label(TextStyle::Caption, t.remotePort, p.textSecondary);
        space(6.0f);
        // The port takes effect when the field loses focus. Applying every
        // keystroke would restart the listener on 9, 98, 987 on the way to 9876.
        if (!remotePortActive_) remotePortEdit_ = config.remoteApi.port;
        ImGui::SetNextItemWidth(px(120.0f));
        ImGui::InputInt("##port", &remotePortEdit_, 0, 0);
        remotePortActive_ = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            config.remoteApi.port = static_cast<std::uint16_t>(std::clamp(remotePortEdit_, 1024, 65535));
        }
        ImGui::EndGroup();
        ImGui::SameLine(half);
        ImGui::BeginGroup();
        textField(t.remoteToken, config.remoteApi.token, "", FieldState::Neutral, "", ImGuiInputTextFlags_Password,
                  half - px(2.0f));
        ImGui::EndGroup();
        space(10.0f);
        if (model.remoteApiRunning) {
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float lineHeight = measure(TextStyle::Caption, "X").y;
            statusDot(ImGui::GetWindowDrawList(), ImVec2(start.x + px(5.0f), start.y + lineHeight * 0.5f + px(1.0f)),
                      p.ready, false);
            ImGui::Indent(px(18.0f));
            label(TextStyle::Caption, format(t.remoteRunning, {std::to_string(config.remoteApi.port)}), p.textSecondary);
            ImGui::Unindent(px(18.0f));
            space(4.0f);
        }
        if (edgeButton(t.remoteDocs, ButtonKind::Link)) actions_.openHelp("remote-control");
    }
}

} // namespace veloplay::ui
