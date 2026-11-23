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
    const char* batteryValue;    // {0} = percent
    const char* batteryCharging; // {0} = percent

    // Consoles
    const char* statusReady;
    const char* statusStandby;
    const char* statusOffline;
    const char* statusViaInternet; // paired, not found here, but Sony's servers list it
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
    const char* tabKeyboard;
    const char* tabControls;
    const char* tabWheel;
    const char* tabCapture;
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
    const char* upscalingNote;        // {0} = method; shown over the picture after F8
    const char* upscalingNotEnlarged; // second line of that note
    const char* upscalingRtxRefused;  // second line of that note
    const char* sharpness;
    const char* sharpnessHint;
    const char* upscalingRtxOnlyNvidia;
    const char* amdFrcUse;
    const char* amdFrcHint;
    const char* amdFrcOnlyAmd;
    const char* upscalingRtxMissing;
    const char* beta;
    const char* captureCards;
    const char* wheelTitle;
    const char* wheelNone;
    const char* wheelChoose;
    const char* wheelChooseHint;
    const char* wheelMissing;
    const char* wheelUse;
    const char* wheelHint;
    const char* wheelSteer;
    const char* wheelGas;
    const char* wheelBrake;
    const char* wheelAssign;
    const char* wheelNotAssigned;
    const char* wheelAxis;   // {0} = number
    const char* wheelButton; // {0} = number
    const char* wheelTurnNow;
    const char* wheelPressPedalNow;
    const char* wheelPressButtonNow;
    const char* wheelDpadHint;
    const char* wheelSteering;
    const char* wheelSteeringHint;
    const char* wheelCentering;
    const char* wheelCenteringHint;
    const char* captureCardsHint;
    const char* captureShow;
    const char* captureStop;
    const char* pauseCaptureLink;
    const char* presenceState;  // the second line of the card in Discord
    const char* presenceButton; // the button others see on it
    const char* discordTitle;
    const char* discordShow;
    const char* discordShowHint;
    const char* discordGame;
    const char* discordGameName;
    const char* discordGameNamePlaceholder;
    const char* discordGameNameHint;
    const char* pauseCaptureControl;
    const char* look;
    const char* lookVivid;
    const char* lookCinema;
    const char* lookHint;
    const char* lookStrength;
    const char* lookNote;
    const char* deband;
    const char* debandHint;
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
    const char* jitterAuto;
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
    const char* captureSaved;  // {0} = folder
    const char* captureShots;
    const char* captureShotsHint;
    const char* captureOpenFolder;
    const char* captureClips;
    const char* captureVideos;
    const char* captureLater;
    const char* captureFailed;
    const char* captureVideosHint;
    const char* captureVideosMissing;
    const char* captureVideoSaved;  // {0} = folder
    const char* captureVideoFailed;
    const char* recordingBadge;     // {0} = minutes:seconds
    const char* recordingStarting;
    const char* installTitle;
    const char* installHint; // {0} = folder
    const char* installButton;
    const char* installBusy;
    const char* installedHint;
    const char* installFailed;
    const char* uninstallRunning;
    const char* uninstallDone;
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
    const char* noticePsnNoAnswer;    // {0} = console
    const char* noticePsnUnreachable; // {0} = console
    const char* noticePsnFailed;      // {0} = console
    const char* accountSignIn;
    const char* accountSignInHint;
    const char* accountSignInBrowser;
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
    const char* pauseOverlaySmall;
    const char* statsCompact;
    const char* statsCorner;
    const char* cornerTopLeft;
    const char* cornerTopRight;
    const char* cornerBottomLeft;
    const char* cornerBottomRight;
    const char* pauseOverlayAll;
    const char* pauseSince;        // {0} = time connected, like 12:34
    const char* pauseTilePicture;
    const char* pauseTileLink;
    const char* pauseTileControl;
    const char* pausePictureValue; // {0} = pictures per second on screen
    const char* pauseLinkGood;
    const char* pauseLinkRough;
    const char* pauseLinkDetail;   // {0} = Mbit/s, {1} = ms, {2} = percent lost
    const char* pauseNoController;
    const char* pauseQuick;
    const char* pauseKeys;
    const char* pauseLayout;
    const char* micTitle;
    const char* micEnable;
    const char* micHint;
    const char* micLive;     // badge over the picture
    const char* micLiveHint; // next to the switch in the menu
    const char* micMissing;
    const char* hudUnavailable;          // frame generation could not be started
    const char* hudInsidePc;             // row label: how long a picture takes from arriving to being drawn
    const char* modeAuto;                // the program chooses what to ask the console for
    const char* modeAutoHint;
    const char* inputAuto;               // switch on the input tab
    const char* inputAutoHint;
    const char* inputAutoHdr;            // what "automatic" does about HDR
    const char* frameGenerationCapped;   // {0} = refresh rate of the screen, {1} = factor used, {2} = input fps
    const char* hudGame;                 // row label: the rate the game itself runs at
    // Notes over the running picture.
    const char* alertNoController;
    const char* alertConnection;
    const char* alertLost;
    const char* alertDecoder;
    const char* alertFewer;
    const char* alertBattery;
    const char* alertGraphicsRestored;
    const char* alertUnsteadyLink;
    const char* alertUnsteadyAway;
    const char* pauseAway;
    const char* awayHelp;
    const char* awayTitle;
    const char* awayHint;
    const char* awaySignIn;
    const char* awaySigningIn;
    const char* awaySignOut;
    const char* awaySignedIn;
    const char* awayConsoles; // {0} = names
    const char* awayNoConsole;
    const char* awayExpired;
    const char* awayFailed;
    const char* awayTryAtHome;
    const char* awayTryAtHomeHint;
    const char* awayKept;
    const char* notesTitle;
    const char* notesShow;
    const char* notesHint;
    const char* notesChoose;
    const char* notesChooseLess;
    const char* waitingForRelease; // the console has not let go of the session before
    // Playing with keyboard and mouse.
    const char* kbmTitle;
    const char* kbmEnable;
    const char* kbmHint;
    const char* kbmSensitivity;
    const char* kbmSensitivityHint;
    const char* kbmInvertY;
    const char* kbmLayout;
    const char* kbmLayoutHint;
    const char* kbmPressKey;     // shown on a control that waits for its key
    const char* kbmUnassigned;
    const char* kbmPresetStandard;
    const char* kbmPresetOpenWorld;
    const char* kbmPresetOwn;
    const char* kbmPresetStandardHint;
    const char* kbmPresetOpenWorldHint;
    const char* kbmPresetOwnHint;
    const char* padHints;
    const char* consolesTitle;
    const char* padTestTitle;
    const char* padTestRate;      // {0} = reports per second, {1} = milliseconds between two
    const char* padTestVibration;
    const char* padTestTriggers;
    const char* padTestResistance;
    const char* padTestPoint;
    const char* padTestShake;
    const char* padTestHint;
    const char* padTestHintPlain;
    const char* padTestBusy;
    const char* kbmFor;
    const char* kbmAllGames;
    const char* kbmAddGame;     // {0} = name of the game
    const char* kbmRemoveGame;  // {0} = name of the game
    const char* kbmGamesHint;
    const char* kbmNoGameHint;
    const char* kbmOverlayTitle;  // {0} = name of the game, or "all games"
    const char* kbmOverlayClose;
    const char* kbmMouseLooks;   // the right stick follows the mouse
    const char* padCross;
    const char* padCircle;
    const char* padSquare;
    const char* padTriangle;
    const char* padDpad;         // {0} = direction
    const char* padLeftStick;    // {0} = direction
    const char* padRightStick;   // {0} = direction
    const char* padUp;
    const char* padDown;
    const char* padLeft;
    const char* padRight;
    const char* padTouchpad;
    const char* padPs;
    const char* keySpace;
    const char* keyShift;
    const char* keyCtrl;
    const char* keyEnter;
    const char* keyBackspace;
    const char* keyArrow;        // {0} = direction
    const char* keyMouseLeft;
    const char* keyMouseRight;
    const char* keyMouseMiddle;
    const char* keyMouseSide;    // {0} = 4 or 5
    const char* keyWheelUp;
    const char* keyWheelDown;
    const char* keyOther;        // {0} = number of the key
};

const Strings& strings(Language language);

/// All texts of a language in declaration order. For tests.
std::span<const char* const> allTexts(const Strings& strings);

/// Replaces {0}, {1}, ... with the given arguments.
std::string format(std::string_view pattern, std::initializer_list<std::string_view> arguments);

} // namespace veloplay::ui
