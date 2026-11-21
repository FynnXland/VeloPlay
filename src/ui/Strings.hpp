#pragma once

#include <initializer_list>
#include <span>
#include <string>
#include <string_view>

// All text the user can see, in German and English. Placeholders are written
// as {0}, {1}. This file has no dependency on the UI toolkit, so a unit test
// can check that both languages are complete and use the same placeholders.

namespace veloplay::ui {

enum class Language { German, English };

/// `setting` is the value from the settings file ("auto", "de", "en"),
/// `systemLanguage` the two-letter code of the operating system language.
Language resolveLanguage(std::string_view setting, std::string_view systemLanguage);

struct Strings {
    // General
    const char* settings;
    const char* back;
    const char* next;
    const char* cancel;
    const char* done;
    const char* retry;
    const char* close;
    const char* off;
    const char* remove;

    // Header
    const char* controllerNone;
    const char* viaUsb;
    const char* viaBluetooth;

    // Consoles
    const char* statusReady;
    const char* statusStandby;
    const char* statusOffline;
    const char* statusWaking;
    const char* nowRunning; // {0} = game title
    const char* notPaired;
    const char* connect;
    const char* wake;
    const char* pair;
    const char* pairAgain;
    const char* forget;
    const char* forgetQuestion; // {0} = console name
    const char* keysUnreadable;
    const char* addConsole;

    // Empty dashboard
    const char* searchTitle;
    const char* searchBody;
    const char* searchCheckPower;
    const char* searchCheckNetwork;
    const char* searchCheckRemotePlay;
    const char* searchCheckFirewall;
    const char* enterAddress;

    // Picture mode
    const char* modeTitle;
    const char* modeSmooth;
    const char* modeNative;
    const char* modeCustom;
    const char* modeSmoothHint;
    const char* modeNativeHint;
    const char* modeCustomHint;

    const char* previewNote; // {0} = version

    // Pairing
    const char* pairingTitle;
    const char* stepOf; // {0} = current step
    const char* stepConsoleTitle;
    const char* stepConsoleBody;
    const char* stepConsoleNone;
    const char* addressLabel;
    const char* addressInvalid;
    const char* stepAccountTitle;
    const char* stepAccountBody;
    const char* accountLabel;
    const char* accountInvalid;
    const char* accountHelp;
    const char* accountPrivacy;
    const char* stepPinTitle;
    const char* stepPinOpen;
    const char* stepPinEnter;
    const char* pinLabel;
    const char* pinInvalid;
    const char* pairingRunning;
    const char* pairingRunningBody;
    const char* pairingDoneTitle; // {0} = console name
    const char* pairingDoneBody;
    const char* pairingFailedTitle;
    const char* failTimeout;
    const char* failRejected;
    const char* failNetwork;
    const char* failOther;
    const char* technicalDetail; // {0} = message

    // Settings
    const char* tabStream;
    const char* tabPicture;
    const char* tabController;
    const char* tabAdvanced;
    const char* inputIntro;  // heads the tab with what the console sends
    const char* outputIntro; // heads the tab with what the PC makes of it
    const char* flowInput;   // caption of the left box in the strip above both tabs
    const char* flowOutput;  // caption of the right box
    const char* flowUpscaled;
    const char* flowSharpened;
    const char* frameRate;
    const char* resolution;
    const char* codec;
    const char* codecHint;
    const char* hdr;
    const char* hdrHint;
    const char* bitrate;
    const char* bitrateValue; // {0} = Mbit/s
    const char* bitrateHint;
    const char* frameGeneration;
    const char* frameGenerationHint;    // {0} = input fps, {1} = output fps, {2} = added delay in ms
    const char* frameGenerationOffHint; // {0} = fps
    const char* upscaling;
    const char* upscalingHint;
    const char* upscalingRtxHint;
    const char* sharpness;
    const char* sharpnessHint;
    const char* vsync;
    const char* vsyncHint;
    const char* activeController;
    const char* automatic;
    const char* controllerAutoHint;
    const char* deadzone;
    const char* deadzoneHint;
    const char* rumble;
    const char* adaptiveTriggers;
    const char* language;
    const char* languageAuto;
    const char* remoteTitle;
    const char* remoteHint;
    const char* remoteEnable;
    const char* remotePort;
    const char* remoteToken;
    const char* remoteRunning; // {0} = port
    const char* remoteDocs;
    const char* jitter;
    const char* jitterLow;
    const char* jitterStable;
    const char* jitterHint;
    const char* statsOverlay;
    const char* extraAddresses;
    const char* extraAddressesHint;
    const char* add;
    const char* graphicsCard; // {0} = name

    // Updates, connection check, in-stream display
    const char* updateAvailable; // {0} = version
    const char* updateInstall;
    const char* updateNotes;
    const char* updateDownloading; // {0} = version, {1} = percent
    const char* updateInstalling; // {0} = version
    const char* updateRestart; // {0} = version
    const char* restartNow;
    const char* updateFailChecksum;
    const char* updateFailNetwork;
    const char* updateFailWrite;
    const char* updateFailArchive;
    const char* openDownloads;
    const char* updatesTitle;
    const char* updatesInstalled; // {0} = version
    const char* updatesAuto;
    const char* updatesAutoHint;
    const char* updatesCheckNow;
    const char* updatesChecking;
    const char* updatesCurrent;
    const char* updatesNone;
    const char* updatesOffline;
    const char* checkConnection;
    const char* connectionTitle; // {0} = console name
    const char* connectionMeasuring;
    const char* connectionMeasuringHint;
    const char* qualityExcellent;
    const char* qualityGood;
    const char* qualityFair;
    const char* qualityPoor;
    const char* qualityExcellentHint;
    const char* qualityGoodHint;
    const char* qualityFairHint;
    const char* qualityPoorHint;
    const char* connectionNoAnswer;
    const char* statResponse;
    const char* statJitter;
    const char* statLoss;
    const char* msValue; // {0} = milliseconds
    const char* percentValue; // {0} = percent
    const char* connectionGraphHint;
    const char* adviceTitle;
    const char* adviceText; // {0} = Mbit/s, {1} = buffer mode
    const char* adviceApply;
    const char* adviceApplied;
    const char* tipsTitle;
    const char* tipBand;
    const char* tipBridge;
    const char* tipPowerline;
    const char* tipQuiet;
    const char* hudConsole;
    const char* hudDisplay;
    const char* hudFrameGeneration;
    const char* hudPaused;
    const char* hudBitrate;
    const char* hudResponse;
    const char* hudLoss;
    const char* hudAddedDelay;
    const char* hudFrameTimes;
    const char* fpsValue; // {0} = frames per second

    // Notices
    const char* noticeWakeTimedOut;          // {0} = console name
    const char* noticeWakeFailed;            // {0} = console name
    const char* noticeConfigSaveFailed;      // {0} = technical message
    const char* noticeConfigLoadFailed;      // {0} = technical message
    const char* noticeDiscoveryFailed;       // {0} = technical message
    const char* noticeRemoteApiFailed;       // {0} = port
    const char* noticeSessionRefused;        // {0} = console name, as in all session notices
    const char* noticeSessionInUse;
    const char* noticeSessionVersionMismatch;
    const char* noticeSessionConsoleCrashed;
    const char* noticeSessionSetupFailed;
    const char* noticeSessionLost;
    const char* noticeSleepFailed;

    // Session
    const char* connecting;        // {0} = console name
    const char* connectingHint;
    const char* connectingAttempt; // {0} = attempt, {1} = attempts
    const char* loginPinTitle;     // {0} = console name
    const char* loginPinBody;
    const char* loginPinWrong;
    const char* loginPinLabel;
    const char* loginPinSubmit;
    const char* streamingTitle;    // {0} = console name
    const char* streamingBody;
    const char* streamingNoController;
    const char* streamingReceived; // {0} = frames, {1} = audio packets, {2} = megabytes
    const char* streamingLost;     // {0} = frames
    const char* disconnect;
    const char* sleepConsole;
    const char* reconnectingTitle; // {0} = console name
    const char* reconnectingBody;  // {0} = attempt, {1} = attempts
    const char* disconnecting;
    const char* adaptiveTriggersHint;
    const char* streamingNoPicture;
    const char* pauseBody;
    const char* resume;
    const char* streamKeys;
    const char* hudDecoder;
    const char* noticeBitrateLowered; // {0} = new rate in Mbit/s
    const char* accountSignIn;
    const char* accountSignInHint;
    const char* accountSigningIn;
    const char* accountFetching;
    const char* accountLookupFailed;
    const char* hudDropped;
    const char* hudDecodeSlowest;
    const char* automatic2;              // "Auto" on a segmented control
    const char* frameGenerationAutoOn;   // {0} = input fps, {1} = output fps, {2} = added delay in ms
    const char* frameGenerationAutoOff;  // {0} = input fps
    const char* inputApplyHint;
    const char* inputApply;
    const char* pauseOverlay;
    const char* hudUnavailable;          // frame generation could not be started
    const char* hudInsidePc;             // row label: how long a picture takes from arriving to being drawn
};

const Strings& strings(Language language);

/// All texts of a language in declaration order. For tests.
std::span<const char* const> allTexts(const Strings& strings);

/// Replaces {0}, {1}, ... with the given arguments.
std::string format(std::string_view pattern, std::initializer_list<std::string_view> arguments);

} // namespace veloplay::ui
