#pragma once

#include <optional>
#include <string>

#include "app/UiModel.hpp"
#include "ui/Strings.hpp"
#include "ui/Theme.hpp"

namespace veloplay::ui {

/// Draws every screen of the client with Dear ImGui. It knows nothing about
/// windows or renderers: the host starts an ImGui frame, calls draw() and
/// renders the result. Everything it shows comes from the UiModel, everything
/// the user does goes out through IUiActions.
class UiManager {
public:
    enum class Sheet { None, Pairing, Settings, Connection };
    enum class PairingStep { Console, Account, Pin, Progress };
    /// Controller, Keyboard and Wheel are the three pages under "Controls", which has a row of its own.
    enum class SettingsTab { Stream, Picture, Controller, Keyboard, Wheel, Capture, Advanced };

    explicit UiManager(IUiActions& actions);

    /// Call after creating the ImGui context and whenever the display scale changes.
    void configure(const Fonts& fonts, float scale, std::string_view systemLanguage);

    /// Feeds controller input into ImGui's navigation. Call before ImGui::NewFrame().
    void feedNavigation(const UiModel& model);

    /// Call between ImGui::NewFrame() and ImGui::Render().
    void draw(const UiModel& model);

    // Entry points used by the dashboard itself and by tools/ui_preview.
    void openPairing(std::optional<std::string> address = std::nullopt);
    void openSettings(SettingsTab tab = SettingsTab::Stream);
    /// Starts measuring the link to a console and shows the result live.
    void openConnection(std::string_view hostId);
    void closeSheet();
    /// Opens the menu over a running picture, as Esc does.
    void showPauseMenu() { pauseMenu_ = true; }
    /// Shows the note that follows a press of F8, for a picture of it.
    void showUpscalingNote() { upscalingNoteFor_ = 1.0e9; }
    /// Shows the key layout over the picture, as F1 does.
    void showKeyOverlay() { keyOverlay_ = true; }
    /// True while the picture of the game is all there is on screen: no
    /// menu, no settings. Keys and mouse can go to the game then.
    [[nodiscard]] bool pictureOwnsInput() const { return pictureOwnsInput_; }
    /// Height in pixels of the bar the screens drew at the top of the window
    /// in the last frame, and how much of its right end the buttons take.
    /// The rest of the bar is where the window is dragged by. Zero while no
    /// bar is shown.
    [[nodiscard]] float windowBarHeight() const { return windowBarHeight_; }
    [[nodiscard]] float windowBarButtonsWidth() const { return windowBarButtonsWidth_; }
    /// Scrolls the open settings to a place on their page, 0 = top, 1 = end.
    /// For pictures of what lies below the fold.
    void scrollSettingsTo(float share) { settingsScrollTo_ = share; }
    /// Opens the menu of a console's row without a click, with the question
    /// whether to remove the pairing if `asking`. For pictures of it.
    void showConsoleMenu(std::string hostId, bool asking) {
        menuToShow_ = std::move(hostId);
        menuToShowAsks_ = asking;
    }
    /// Opens the settings from that menu, over the running picture.
    void showSettingsInGame(SettingsTab tab) {
        pauseMenu_ = true;
        settingsFromGame_ = true;
        openSettings(tab);
    }
    void setPairingStep(PairingStep step) { pairingStep_ = step; }
    void setPairingInputs(std::string address, std::string account, std::string pin);

private:
    void drawDashboard(const UiModel& model);
    void drawHeader(const UiModel& model);
    void drawUpdateBanner(const UiModel& model);
    void drawNotices(const UiModel& model);
    void drawConnection(const UiModel& model);
    void drawStreamOverlay(const UiModel& model);
    void drawStreamAlerts(const UiModel& model);
    void drawPauseMenu(const UiModel& model);
    void drawUpscalingNote(const UiModel& model);
    void drawCaptureNote(const UiModel& model);
    void drawRecordingBadge(const UiModel& model);
    void drawCaptureCards(const UiModel& model);
    void drawWheel(const UiModel& model, AppConfig& config);
    void drawCaptureTab(const UiModel& model);
    /// Where the display goes for its size, in the corner the settings name.
    [[nodiscard]] ImVec2 statsCornerAt(const UiModel& model, ImVec2 size) const;
    void drawKeyOverlay(const UiModel& model);
    void drawMicrophoneBadge(const UiModel& model);
    /// `shown` 0..1 slides it in from above, for the bar that comes over the picture.
    void drawWindowBar(const UiModel& model, float shown = 1.0f);
    void drawSession(const UiModel& model);
    [[nodiscard]] std::string number(float value, int decimals) const;
    void drawEmptyState(const UiModel& model);
    void drawConsoleRow(const UiModel& model, const ConsoleEntry& console, int index);
    void drawModePicker(const UiModel& model);
    void drawSheets(const UiModel& model);
    void drawPairing(const UiModel& model);
    void drawPairingProgress(const UiModel& model);
    void drawSettings(const UiModel& model);
    void drawStreamTab(const UiModel& model, AppConfig& config);
    void drawPictureTab(const UiModel& model, AppConfig& config);
    void drawControllerTab(const UiModel& model, AppConfig& config);
    void drawKeyboardMouse(const UiModel& model, AppConfig& config);
    void drawControllerTest(const UiModel& model);
    void drawAdvancedTab(const UiModel& model, AppConfig& config);
    bool beginSheet(const char* id, float width);
    void endSheet();

    IUiActions& actions_;
    std::string systemLanguage_ = "en";
    const Strings* text_ = nullptr;
    bool german_ = false;

    Sheet sheet_ = Sheet::None;
    Sheet openedSheet_ = Sheet::None; // which popup ImGui currently has open
    int sheetGeneration_ = 0;

    PairingStep pairingStep_ = PairingStep::Console;
    std::string pairingAddress_;
    std::string pairingAccount_;
    std::string pairingPin_;
    bool accountPrefilled_ = false;

    SettingsTab settingsTab_ = SettingsTab::Stream;
    SettingsTab controlsTab_ = SettingsTab::Controller; // the page under "Controls" seen last
    std::string newAddress_;
    int remotePortEdit_ = 0;
    bool remotePortActive_ = false;
    std::string forgetCandidate_;
    float consoleMenuHeight_ = 0.0f;  // of the menu of a console's row, as it was in the frame before
    std::string menuToShow_;          // a console whose menu is to open by itself (for pictures of it)
    bool menuToShowAsks_ = false;
    bool adviceApplied_ = false;
    bool pauseMenu_ = false; // the menu over a running picture, opened with Esc
    bool pictureOwnsInput_ = false;
    float windowBarHeight_ = 0.0f;
    bool barPeek_ = false; // the bar is over the picture because the pointer went to the top edge
    float windowBarButtonsWidth_ = 0.0f;
    float menuShown_ = 1.0f; // 0 to 1 while the menu over the picture fades in
    double upscalingNoteFor_ = 0.0; // seconds the note after F8 stays; counts down
    double captureNoteFor_ = 0.0;   // the same for the note after a screenshot
    int captureNoteSeen_ = 0;       // the serial of the last note shown
    float settingsScrollTo_ = -1.0f; // negative: wherever the user scrolled to
    int bindingControl_ = -1; // the control that waits for its key in the settings, or -1
    std::string editedGameId_;      // the game whose key layout the settings show; empty = the one for all games
    bool editedGameChosen_ = false; // false until the settings picked the layout in use
    bool keyOverlay_ = false;       // the key layout is shown over the picture (F1)
    int triggerTest_ = 0;           // the test effect on the adaptive triggers, 0 = none
    bool controllerTabDrawn_ = false; // the controller settings were on screen in the frame before
    bool settingsFromGame_ = false; // the settings sheet was opened from that menu
    std::string loginPin_; // typed login passcode; never leaves this field except to the session
};

} // namespace veloplay::ui
