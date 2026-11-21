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
    enum class SettingsTab { Stream, Picture, Controller, Advanced };

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
    std::string newAddress_;
    int remotePortEdit_ = 0;
    bool remotePortActive_ = false;
    std::string forgetCandidate_;
    bool adviceApplied_ = false;
    bool pauseMenu_ = false; // the menu over a running picture, opened with Esc
    bool settingsFromGame_ = false; // the settings sheet was opened from that menu
    std::string loginPin_; // typed login passcode; never leaves this field except to the session
};

} // namespace veloplay::ui
