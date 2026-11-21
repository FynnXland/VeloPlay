#include "ui/Strings.hpp"

namespace veloplay::ui {

namespace {

constexpr Strings kGerman = {
    .settings = "Einstellungen",
    .back = "Zurück",
    .next = "Weiter",
    .cancel = "Abbrechen",
    .done = "Fertig",
    .retry = "Erneut versuchen",
    .close = "Schließen",
    .off = "Aus",
    .remove = "Entfernen",

    .controllerNone = "Kein Controller verbunden",
    .viaUsb = "USB",
    .viaBluetooth = "Bluetooth",

    .statusReady = "Bereit",
    .statusStandby = "Im Ruhemodus",
    .statusOffline = "Nicht erreichbar",
    .statusWaking = "Wird aufgeweckt",
    .nowRunning = "{0} läuft",
    .notPaired = "Nicht gekoppelt",
    .connect = "Verbinden",
    .wake = "Aufwecken",
    .pair = "Koppeln",
    .pairAgain = "Neu koppeln",
    .forget = "Kopplung entfernen",
    .forgetQuestion = "Kopplung mit {0} entfernen?",
    .keysUnreadable = "Die gespeicherten Schlüssel gehören zu einem anderen Windows-Konto. Kopple die Konsole neu.",
    .addConsole = "Konsole hinzufügen",

    .searchTitle = "Suche nach deiner PS5",
    .searchBody = "Das dauert meist nur ein paar Sekunden. Falls nichts erscheint, prüfe diese Punkte:",
    .searchCheckPower = "Die PS5 ist eingeschaltet oder im Ruhemodus.",
    .searchCheckNetwork = "PS5 und PC hängen im selben Netzwerk.",
    .searchCheckRemotePlay = "Remote Play ist auf der PS5 aktiviert: Einstellungen > System > Remote Play.",
    .searchCheckFirewall = "Die Windows-Firewall erlaubt VeloPlay den Zugriff auf private Netzwerke.",
    .enterAddress = "IP-Adresse eingeben",

    .modeTitle = "Bildmodus",
    .modeSmooth = "30-fps-Spiel",
    .modeNative = "60-fps-Spiel",
    .modeCustom = "Eigene Werte",
    .modeSmoothHint = "Die PS5 sendet 30 Bilder pro Sekunde. Dein PC berechnet Zwischenbilder und zeigt 60.",
    .modeNativeHint = "Die PS5 sendet 60 Bilder pro Sekunde. Sie werden unverändert angezeigt.",
    .modeCustomHint = "Es gelten deine Werte aus den Einstellungen.",

    .previewNote = "Vorschau {0}. Zwischenbilder und Hochskalierung sind neu und noch nicht an vielen Grafikkarten "
                   "erprobt. Sieht etwas falsch aus, stell Zwischenbilder unter Einstellungen > Ausgabe auf Aus.",

    .pairingTitle = "Konsole koppeln",
    .stepOf = "Schritt {0} von 3",
    .stepConsoleTitle = "Welche Konsole?",
    .stepConsoleBody = "Wähle deine PS5 aus der Liste. Fehlt sie, gib ihre IP-Adresse ein. Die PS5 zeigt sie unter "
                       "Einstellungen > Netzwerk > Verbindungsstatus.",
    .stepConsoleNone = "Im Netzwerk wurde noch keine PS5 gefunden.",
    .addressLabel = "IP-Adresse",
    .addressInvalid = "Eine IP-Adresse besteht aus vier Zahlen mit Punkten, zum Beispiel 192.168.1.20.",
    .stepAccountTitle = "Dein PSN-Konto",
    .stepAccountBody = "Die PS5 koppelt nur Geräte, die zu einem ihrer Konten gehören. VeloPlay braucht dafür deine "
                       "Account-ID. Das ist eine feste Nummer deines Kontos und nicht dein Online-Name.",
    .accountLabel = "Account-ID",
    .accountInvalid = "Das passt nicht. Die Account-ID hat entweder 12 Zeichen und endet mit = oder ist eine Zahl mit "
                      "etwa 19 Stellen.",
    .accountHelp = "Wie finde ich meine Account-ID?",
    .accountPrivacy = "Die ID bleibt auf diesem PC und geht nur an deine PS5.",
    .stepPinTitle = "PIN von der PS5",
    .stepPinOpen = "Öffne auf der PS5 Einstellungen > System > Remote Play > Gerät verknüpfen.",
    .stepPinEnter = "Die PS5 zeigt eine achtstellige Zahl. Gib sie hier ein und lass den Bildschirm auf der PS5 geöffnet.",
    .pinLabel = "PIN",
    .pinInvalid = "Die PIN hat genau acht Ziffern.",
    .pairingRunning = "Kopplung läuft",
    .pairingRunningBody = "VeloPlay tauscht gerade die Schlüssel mit der PS5 aus.",
    .pairingDoneTitle = "{0} ist gekoppelt",
    .pairingDoneBody = "Die Konsole steht jetzt in der Übersicht. Die PIN brauchst du nicht mehr.",
    .pairingFailedTitle = "Kopplung fehlgeschlagen",
    .failTimeout = "Die PS5 hat nicht geantwortet. Der Bildschirm „Gerät verknüpfen“ muss geöffnet bleiben, und die "
                   "IP-Adresse muss stimmen.",
    .failRejected = "Die PS5 hat abgelehnt. Meist ist die PIN vertippt oder abgelaufen, oder die Account-ID gehört zu "
                    "einem Konto, das auf dieser PS5 nicht angemeldet ist.",
    .failNetwork = "Die Verbindung zur PS5 ist abgebrochen. Prüfe das Netzwerk und versuche es noch einmal.",
    .failOther = "Die Antwort der PS5 war unerwartet. Die Details stehen in der Logdatei.",
    .technicalDetail = "Technische Meldung: {0}",

    .tabStream = "Eingang",
    .tabPicture = "Ausgabe",
    .tabController = "Controller",
    .tabAdvanced = "Erweitert",
    .inputIntro = "Das schickt die PS5. VeloPlay fordert diese Werte beim Verbinden von der Konsole an. Änderungen "
                  "gelten ab der nächsten Verbindung.",
    .outputIntro = "Das macht dein PC aus dem Bild der PS5, bevor es auf dem Bildschirm erscheint. Änderungen gelten "
                   "sofort.",
    .flowInput = "PS5 sendet",
    .flowOutput = "Bildschirm zeigt",
    .flowUpscaled = "hochskaliert",
    .flowSharpened = "geschärft",
    .frameRate = "Bildrate der PS5",
    .resolution = "Auflösung",
    .codec = "Videocodec",
    .codecHint = "HEVC liefert bei gleicher Datenrate das bessere Bild. H.264 hilft bei sehr alten Grafikkarten.",
    .hdr = "HDR",
    .hdrHint = "HDR wird noch nicht angezeigt. Der Schalter wirkt ab einer späteren Version.",
    .bitrate = "Datenrate",
    .bitrateValue = "{0} Mbit/s",
    .bitrateHint = "Per Netzwerkkabel sind 30 bis 50 Mbit/s sinnvoll, im WLAN eher 10 bis 20.",
    .frameGeneration = "Zwischenbilder",
    .frameGenerationHint = "Aus {0} fps von der PS5 werden {1} fps auf dem Bildschirm. Dein PC berechnet die Bilder "
                           "dazwischen. Das Bild erscheint dafür etwa {2} ms später.",
    .frameGenerationOffHint = "Das Bild wird mit {0} fps angezeigt, so wie die PS5 es sendet.",
    .upscaling = "Hochskalierung",
    .upscalingHint = "Lanczos rechnet das Bild mit einem schärferen Filter auf die Größe des Fensters hoch. Aus: Das "
                     "Bild wird weich vergrößert.",
    .upscalingRtxHint = "RTX Video Super Resolution vergrößert das Bild mit einem neuronalen Netz deiner NVIDIA-Karte "
                        "und glättet dabei Klötzchen aus der Übertragung. Dafür muss in der NVIDIA-App unter System > "
                        "Video die RTX-Video-Verbesserung mit Superauflösung eingeschaltet sein. VeloPlay kann nicht "
                        "erkennen, ob der Treiber sie anwendet; die NVIDIA-App zeigt es dort an.",
    .sharpness = "Schärfe",
    .sharpnessHint = "Hebt Kanten nach dem Vergrößern wieder hervor. 0 % lässt das Bild, wie es ist.",
    .vsync = "VSync",
    .vsyncHint = "Verhindert zerrissene Bilder. Mit G-Sync oder FreeSync kannst du es ausschalten.",
    .activeController = "Controller",
    .automatic = "Automatisch",
    .controllerAutoHint = "VeloPlay nimmt einen DualSense am Kabel, wenn einer angeschlossen ist.",
    .deadzone = "Totzone der Sticks",
    .deadzoneHint = "Erhöhe den Wert, wenn sich die Kamera von allein bewegt.",
    .rumble = "Vibration",
    .adaptiveTriggers = "Adaptive Trigger",
    .language = "Sprache",
    .languageAuto = "Wie Windows",
    .remoteTitle = "Fernsteuerung für Skripte",
    .remoteHint = "Programme auf diesem PC können Tasten senden und den Status abfragen. Von anderen Geräten aus ist "
                  "die Schnittstelle nicht erreichbar.",
    .remoteEnable = "Fernsteuerung einschalten",
    .remotePort = "Port",
    .remoteToken = "Kennwort (optional)",
    .remoteRunning = "Aktiv auf 127.0.0.1:{0}",
    .remoteDocs = "Befehle ansehen",
    .jitter = "Netzwerkpuffer",
    .jitterLow = "Geringe Latenz",
    .jitterStable = "Stabil",
    .jitterHint = "„Stabil“ gleicht Schwankungen im WLAN aus und kostet einige Millisekunden.",
    .statsOverlay = "Statistik im Stream anzeigen",
    .extraAddresses = "Zusätzliche Adressen",
    .extraAddressesHint = "Für Konsolen, die die automatische Suche nicht findet, etwa in einem anderen Netzwerksegment.",
    .add = "Hinzufügen",
    .graphicsCard = "Grafikkarte: {0}",

    .updateAvailable = "Version {0} ist da.",
    .updateInstall = "Aktualisieren",
    .updateNotes = "Was ist neu?",
    .updateDownloading = "Version {0} wird geladen: {1} %",
    .updateInstalling = "Version {0} wird eingerichtet.",
    .updateRestart = "Version {0} ist eingerichtet. Nach einem Neustart läuft sie.",
    .restartNow = "Neu starten",
    .updateFailChecksum = "Der Download war beschädigt und wurde verworfen. Versuch es noch einmal.",
    .updateFailNetwork = "Der Download ist abgebrochen. Prüfe die Internetverbindung und versuch es noch einmal.",
    .updateFailWrite = "VeloPlay darf seinen Ordner nicht ändern. Lade die neue Version von der Download-Seite und ersetze den Ordner.",
    .updateFailArchive = "Die neue Version ließ sich nicht entpacken. Lade sie von der Download-Seite.",
    .openDownloads = "Download-Seite öffnen",
    .updatesTitle = "Updates",
    .updatesInstalled = "Installiert ist Version {0}.",
    .updatesAuto = "Beim Start nach Updates suchen",
    .updatesAutoHint = "VeloPlay fragt dafür bei GitHub nach der neuesten Version. Sonst wird nichts übertragen.",
    .updatesCheckNow = "Jetzt suchen",
    .updatesChecking = "Suche läuft",
    .updatesCurrent = "Du hast die neueste Version.",
    .updatesNone = "Es ist noch keine Version veröffentlicht.",
    .updatesOffline = "GitHub ist gerade nicht erreichbar.",
    .checkConnection = "Verbindung prüfen",
    .connectionTitle = "Verbindung zu {0}",
    .connectionMeasuring = "Messung läuft",
    .connectionMeasuringHint = "VeloPlay schickt der Konsole kleine Pakete und misst, wie schnell und wie gleichmäßig sie antwortet.",
    .qualityExcellent = "Sehr gut",
    .qualityGood = "Gut",
    .qualityFair = "Mittel",
    .qualityPoor = "Schlecht",
    .qualityExcellentHint = "So ruhig wie eine Kabelverbindung. Hohe Bildqualität ist kein Problem.",
    .qualityGoodHint = "Gut zum Spielen. Die Antwortzeiten schwanken kaum.",
    .qualityFairHint = "Spielbar, mit gelegentlichen Rucklern oder kurzer Eingabeverzögerung.",
    .qualityPoorHint = "Hier ist mit Rucklern und spürbarer Eingabeverzögerung zu rechnen.",
    .connectionNoAnswer = "Die Konsole antwortet nicht.",
    .statResponse = "Antwortzeit",
    .statJitter = "Schwankung",
    .statLoss = "Verlust",
    .msValue = "{0} ms",
    .percentValue = "{0} %",
    .connectionGraphHint = "Jeder Balken ist eine Messung. Gleich hohe Balken sind gut, rote Striche sind verlorene Pakete.",
    .adviceTitle = "Empfohlene Einstellung",
    .adviceText = "{0} Mbit/s, Netzwerkpuffer „{1}“",
    .adviceApply = "Übernehmen",
    .adviceApplied = "Übernommen",
    .tipsTitle = "Was die Verbindung ruhiger macht",
    .tipBand = "Auf der PS5 das WLAN-Frequenzband fest einstellen und beide ausprobieren: 5 GHz ist schneller, 2,4 GHz reicht weiter. Miss nach jedem Wechsel neu.",
    .tipBridge = "Einen WLAN-Repeater oder Mesh-Punkt mit LAN-Buchse neben die PS5 stellen und die PS5 per kurzem Kabel anschließen.",
    .tipPowerline = "Powerline-Adapter leiten das Netzwerk über die Stromleitung. Über mehrere Etagen ist das oft ruhiger als WLAN.",
    .tipQuiet = "Beim Spielen große Downloads auf der PS5 und auf anderen Geräten pausieren.",
    .hudConsole = "PS5",
    .hudDisplay = "Bildschirm",
    .hudFrameGeneration = "Zwischenbilder",
    .hudPaused = "pausiert",
    .hudBitrate = "Datenrate",
    .hudResponse = "Antwortzeit",
    .hudLoss = "Verlust",
    .hudAddedDelay = "Zusatzverzögerung",
    .hudFrameTimes = "Bildabstände",
    .fpsValue = "{0} fps",

    .noticeWakeTimedOut = "{0} ist nicht aufgewacht. Prüfe auf der PS5 unter Einstellungen > System > Energie sparen > "
                          "Im Ruhemodus verfügbare Funktionen, ob „Mit dem Internet verbunden bleiben“ und „Einschalten "
                          "der PS5 aus dem Netzwerk aktivieren“ eingeschaltet sind.",
    .noticeWakeFailed = "{0} lässt sich nicht aufwecken, weil keine Adresse bekannt ist.",
    .noticeConfigSaveFailed = "Die Einstellungen konnten nicht gespeichert werden. {0}",
    .noticeConfigLoadFailed = "Die Einstellungsdatei ist beschädigt. VeloPlay startet mit Standardwerten. {0}",
    .noticeDiscoveryFailed = "Die Netzwerksuche konnte nicht starten. {0}",
    .noticeRemoteApiFailed = "Port {0} ist belegt. Die Fernsteuerung bleibt aus, bis du einen anderen Port wählst.",
    .noticeSessionRefused = "{0} nimmt keine Verbindung an. Prüfe, ob die PS5 eingeschaltet ist und ob unter "
                            "Einstellungen > System > Remote Play „Remote Play aktivieren“ eingeschaltet ist.",
    .noticeSessionInUse = "{0} wird gerade von einem anderen Gerät per Remote Play genutzt. Beende die Verbindung "
                          "dort und versuch es noch einmal.",
    .noticeSessionVersionMismatch = "{0} verwendet eine Remote-Play-Version, die VeloPlay noch nicht kennt. Sieh in "
                                    "den Einstellungen unter Erweitert nach, ob es ein Update gibt.",
    .noticeSessionConsoleCrashed = "Remote Play ist auf {0} abgestürzt. Starte die PS5 neu und versuch es noch einmal.",
    .noticeSessionSetupFailed = "Die Verbindung zu {0} kam nicht zustande. Prüfe, ob die PS5 eingeschaltet und Remote "
                                "Play aktiviert ist. Wenn das wiederholt passiert, kopple die Konsole neu.",
    .noticeSessionLost = "Die Verbindung zu {0} ist abgebrochen und ließ sich nicht wiederherstellen. Über das Menü "
                         "der Konsole kannst du mit „Verbindung prüfen“ das Netzwerk messen.",
    .noticeSleepFailed = "{0} hat den Befehl für den Ruhemodus nicht erhalten. Die Verbindung wurde trotzdem getrennt.",

    .connecting = "Verbinde mit {0}",
    .connectingHint = "Das dauert meist wenige Sekunden.",
    .connectingAttempt = "Die Konsole startet noch. Versuch {0} von {1}.",
    .loginPinTitle = "{0} fragt nach deinem Anmelde-Code",
    .loginPinBody = "Das ist der vierstellige Code, mit dem dein Benutzer auf der PS5 geschützt ist. VeloPlay "
                    "speichert ihn nicht.",
    .loginPinWrong = "Der Code war falsch. Versuch es noch einmal.",
    .loginPinLabel = "Anmelde-Code",
    .loginPinSubmit = "Anmelden",
    .streamingTitle = "Verbunden mit {0}",
    .streamingBody = "Die Verbindung steht. Das erste Bild ist unterwegs.",
    .streamingNoController = "Es ist kein Controller angeschlossen. Schließ einen an, um die PS5 zu steuern.",
    .streamingReceived = "Empfangen: {0} Bilder, {1} Tonpakete, {2} MB",
    .streamingLost = "Verlorene Bilder: {0}",
    .disconnect = "Trennen",
    .sleepConsole = "PS5 in den Ruhemodus",
    .reconnectingTitle = "Verbindung zu {0} unterbrochen",
    .reconnectingBody = "VeloPlay verbindet neu. Versuch {0} von {1}.",
    .disconnecting = "Verbindung wird getrennt",
    .adaptiveTriggersHint = "Der Widerstand von L2 und R2, wie ihn das Spiel vorgibt. Braucht einen DualSense.",
    .streamingNoPicture = "Es kommt kein Bild an, oder dein PC kann es nicht entpacken. Den Grund nennt die Datei "
                          "veloplay.log im Ordner %APPDATA%\\VeloPlay. Der Controller steuert die PS5 trotzdem.",
    .pauseBody = "Das Spiel läuft weiter, und dein Controller steuert weiter die PS5. Diese Knöpfe bedienst du mit der "
                 "Maus.",
    .resume = "Weiter",
    .streamKeys = "Esc: Menü  ·  F11: Vollbild  ·  F9: Anzeige",
    .hudDecoder = "Dekoder",
    .noticeBitrateLowered = "Bei der eingestellten Datenrate kam kein Bild mehr an. VeloPlay hat neu verbunden und sie "
                            "für diese Sitzung auf {0} Mbit/s gesenkt. Dauerhaft änderst du sie unter Einstellungen > "
                            "Stream.",    .accountSignIn = "Bei PlayStation anmelden",
    .accountSignInHint = "Öffnet die Anmeldeseite von Sony in einem eigenen Fenster. Du meldest dich dort an, VeloPlay "
                         "liest danach deine Account-ID aus. Dein Passwort sieht und speichert VeloPlay nicht.",
    .accountSigningIn = "Warte auf deine Anmeldung bei Sony. Hat sich dafür dein Browser geöffnet, kopiere "
                        "nach der Anmeldung die Adresse der leeren Seite und füge sie oben ein.",
    .accountFetching = "Frage Sony nach deiner Account-ID.",
    .accountLookupFailed = "Die Account-ID ließ sich nicht abrufen. Versuch es noch einmal oder trag sie von Hand ein.",
    .hudDropped = "Verworfen",
    .hudDecodeSlowest = "Langsamstes Bild",
    .automatic2 = "Auto",
    .frameGenerationAutoOn = "Automatisch: Die PS5 sendet {0} fps, dein PC berechnet die Bilder dazwischen und zeigt {1} fps. "
                             "Das Bild erscheint dafür etwa {2} ms später. Einen Stream mit 60 fps zeigt VeloPlay, wie er "
                             "ankommt.",
    .frameGenerationAutoOff = "Automatisch: Die PS5 sendet {0} fps. Läuft dein Spiel mit 60, wird es unverändert "
                              "gezeigt. Läuft es mit 30, berechnet dein PC die Bilder dazwischen.",
    .inputApplyHint = "Die PS5 sendet noch mit den alten Werten. Die neuen gelten nach dem Neuverbinden. Das Bild setzt "
                      "dabei ein paar Sekunden aus, das Spiel läuft weiter.",
    .inputApply = "Jetzt neu verbinden",
    .pauseOverlay = "Anzeige oben links",
    .hudUnavailable = "nicht verfügbar",
    .hudInsidePc = "Im PC",
    .modeAuto = "Automatisch",
    .modeAutoHint = "VeloPlay wählt Auflösung, Bildrate und Datenrate selbst und erkennt, ob dein Spiel mit 30 oder "
                    "60 Bildern pro Sekunde läuft.",
    .inputAuto = "Automatisch",
    .inputAutoHint = "VeloPlay fordert die beste Auflösung mit 60 Bildern pro Sekunde an. Läuft dein Spiel nur mit 30, "
                     "erkennt VeloPlay das und berechnet die Bilder dazwischen. Die Datenrate merkt sich VeloPlay pro "
                     "PS5: Nach einer Sitzung ohne Aussetzer gibt es beim nächsten Mal etwas mehr, nach Aussetzern "
                     "weniger.",
    .inputAutoHdr = "HDR wird noch nicht angezeigt und bleibt deshalb aus.",
    .frameGenerationCapped = "Dein Bildschirm zeigt {0} Bilder pro Sekunde. Bei {2} fps von der PS5 passen nur {1}x "
                             "hinein, mehr würde das Bild nur verzögern. VeloPlay rechnet deshalb mit {1}x.",
    .hudGame = "Spiel",
    .alertNoController = "Kein Controller verbunden",
    .alertConnection = "Die Verbindung hakt",
    .alertLost = "Bilder gehen unterwegs verloren",
    .alertDecoder = "Dein PC kommt mit dem Entpacken der Bilder nicht nach",
    .alertFewer = "Die PS5 schickt gerade weniger Bilder",
};

constexpr Strings kEnglish = {
    .settings = "Settings",
    .back = "Back",
    .next = "Continue",
    .cancel = "Cancel",
    .done = "Done",
    .retry = "Try again",
    .close = "Close",
    .off = "Off",
    .remove = "Remove",

    .controllerNone = "No controller connected",
    .viaUsb = "USB",
    .viaBluetooth = "Bluetooth",

    .statusReady = "Ready",
    .statusStandby = "In rest mode",
    .statusOffline = "Not reachable",
    .statusWaking = "Waking up",
    .nowRunning = "Running {0}",
    .notPaired = "Not paired",
    .connect = "Connect",
    .wake = "Wake up",
    .pair = "Pair",
    .pairAgain = "Pair again",
    .forget = "Remove pairing",
    .forgetQuestion = "Remove the pairing with {0}?",
    .keysUnreadable = "The stored keys belong to another Windows account. Pair the console again.",
    .addConsole = "Add console",

    .searchTitle = "Looking for your PS5",
    .searchBody = "This usually takes a few seconds. If nothing shows up, check the following:",
    .searchCheckPower = "The PS5 is switched on or in rest mode.",
    .searchCheckNetwork = "PS5 and PC are on the same network.",
    .searchCheckRemotePlay = "Remote Play is enabled on the PS5: Settings > System > Remote Play.",
    .searchCheckFirewall = "Windows Firewall allows VeloPlay on private networks.",
    .enterAddress = "Enter IP address",

    .modeTitle = "Picture mode",
    .modeSmooth = "30 fps game",
    .modeNative = "60 fps game",
    .modeCustom = "Custom",
    .modeSmoothHint = "The PS5 sends 30 frames per second. Your PC computes the frames in between and shows 60.",
    .modeNativeHint = "The PS5 sends 60 frames per second. They are shown as they arrive.",
    .modeCustomHint = "Your own values from the settings apply.",

    .previewNote = "Preview {0}. Frame generation and upscaling are new and have not been tried on many graphics "
                   "cards yet. If something looks wrong, set frame generation to off under Settings > Output.",

    .pairingTitle = "Pair a console",
    .stepOf = "Step {0} of 3",
    .stepConsoleTitle = "Which console?",
    .stepConsoleBody = "Pick your PS5 from the list. If it is missing, enter its IP address. The PS5 shows it under "
                       "Settings > Network > Connection Status.",
    .stepConsoleNone = "No PS5 has been found on the network yet.",
    .addressLabel = "IP address",
    .addressInvalid = "An IP address has four numbers separated by dots, for example 192.168.1.20.",
    .stepAccountTitle = "Your PSN account",
    .stepAccountBody = "The PS5 only pairs devices that belong to one of its accounts. VeloPlay needs your account ID "
                       "for that. It is a fixed number tied to your account, not your online name.",
    .accountLabel = "Account ID",
    .accountInvalid = "That does not fit. The account ID is either 12 characters ending in = or a number of about "
                      "19 digits.",
    .accountHelp = "How do I find my account ID?",
    .accountPrivacy = "The ID stays on this PC and is only sent to your PS5.",
    .stepPinTitle = "PIN from the PS5",
    .stepPinOpen = "On the PS5, open Settings > System > Remote Play > Link Device.",
    .stepPinEnter = "The PS5 shows an eight-digit number. Enter it here and keep that screen open on the PS5.",
    .pinLabel = "PIN",
    .pinInvalid = "The PIN has exactly eight digits.",
    .pairingRunning = "Pairing",
    .pairingRunningBody = "VeloPlay is exchanging keys with the PS5.",
    .pairingDoneTitle = "{0} is paired",
    .pairingDoneBody = "The console is now on the overview. You no longer need the PIN.",
    .pairingFailedTitle = "Pairing failed",
    .failTimeout = "The PS5 did not answer. Its Link Device screen has to stay open, and the IP address has to be right.",
    .failRejected = "The PS5 said no. Usually the PIN was mistyped or has expired, or the account ID belongs to an "
                    "account that is not signed in on this PS5.",
    .failNetwork = "The connection to the PS5 broke off. Check the network and try again.",
    .failOther = "The PS5 sent an unexpected answer. The log file has the details.",
    .technicalDetail = "Technical message: {0}",

    .tabStream = "Input",
    .tabPicture = "Output",
    .tabController = "Controller",
    .tabAdvanced = "Advanced",
    .inputIntro = "What the PS5 sends. VeloPlay asks the console for these values when it connects. Changes apply "
                  "from the next connection.",
    .outputIntro = "What your PC makes of the PS5's picture before it reaches the screen. Changes apply at once.",
    .flowInput = "PS5 sends",
    .flowOutput = "Screen shows",
    .flowUpscaled = "upscaled",
    .flowSharpened = "sharpened",
    .frameRate = "Frame rate from the PS5",
    .resolution = "Resolution",
    .codec = "Video codec",
    .codecHint = "HEVC gives the better picture at the same bitrate. H.264 helps with very old graphics cards.",
    .hdr = "HDR",
    .hdrHint = "HDR is not shown yet. The switch takes effect in a later version.",
    .bitrate = "Bitrate",
    .bitrateValue = "{0} Mbit/s",
    .bitrateHint = "With a network cable 30 to 50 Mbit/s work well, on Wi-Fi rather 10 to 20.",
    .frameGeneration = "Frame generation",
    .frameGenerationHint = "{0} fps from the PS5 become {1} fps on screen. Your PC computes the frames in between. "
                           "The picture appears about {2} ms later in return.",
    .frameGenerationOffHint = "The picture is shown at {0} fps, as the PS5 sends it.",
    .upscaling = "Upscaling",
    .upscalingHint = "Lanczos scales the picture to the size of the window with a sharper filter. Off: the picture is "
                     "enlarged softly.",
    .upscalingRtxHint = "RTX Video Super Resolution enlarges the picture with a neural network on your NVIDIA card and "
                        "smooths compression blocks while doing so. It needs RTX Video Enhancement with Super "
                        "Resolution switched on in the NVIDIA app under System > Video. VeloPlay cannot tell whether "
                        "the driver applies it; the NVIDIA app shows that there.",
    .sharpness = "Sharpness",
    .sharpnessHint = "Brings edges back out after enlarging. 0 % leaves the picture as it is.",
    .vsync = "VSync",
    .vsyncHint = "Prevents tearing. With G-Sync or FreeSync you can turn it off.",
    .activeController = "Controller",
    .automatic = "Automatic",
    .controllerAutoHint = "VeloPlay picks a DualSense on a cable if one is plugged in.",
    .deadzone = "Stick dead zone",
    .deadzoneHint = "Raise this if the camera moves on its own.",
    .rumble = "Vibration",
    .adaptiveTriggers = "Adaptive triggers",
    .language = "Language",
    .languageAuto = "Same as Windows",
    .remoteTitle = "Remote control for scripts",
    .remoteHint = "Programs on this PC can send button presses and query the status. Other devices cannot reach the "
                  "interface.",
    .remoteEnable = "Turn on remote control",
    .remotePort = "Port",
    .remoteToken = "Password (optional)",
    .remoteRunning = "Listening on 127.0.0.1:{0}",
    .remoteDocs = "View commands",
    .jitter = "Network buffer",
    .jitterLow = "Low latency",
    .jitterStable = "Stable",
    .jitterHint = "\"Stable\" evens out Wi-Fi hiccups and costs a few milliseconds.",
    .statsOverlay = "Show statistics while streaming",
    .extraAddresses = "Additional addresses",
    .extraAddressesHint = "For consoles that the automatic search does not find, for example on another network segment.",
    .add = "Add",
    .graphicsCard = "Graphics card: {0}",

    .updateAvailable = "Version {0} is out.",
    .updateInstall = "Update",
    .updateNotes = "What's new?",
    .updateDownloading = "Downloading version {0}: {1} %",
    .updateInstalling = "Installing version {0}.",
    .updateRestart = "Version {0} is installed. It runs after a restart.",
    .restartNow = "Restart",
    .updateFailChecksum = "The download was damaged and has been discarded. Try again.",
    .updateFailNetwork = "The download broke off. Check the internet connection and try again.",
    .updateFailWrite = "VeloPlay may not change its folder. Get the new version from the download page and replace the folder.",
    .updateFailArchive = "The new version could not be unpacked. Get it from the download page.",
    .openDownloads = "Open download page",
    .updatesTitle = "Updates",
    .updatesInstalled = "Version {0} is installed.",
    .updatesAuto = "Check for updates at startup",
    .updatesAutoHint = "For this VeloPlay asks GitHub for the latest version. Nothing else is sent.",
    .updatesCheckNow = "Check now",
    .updatesChecking = "Checking",
    .updatesCurrent = "You have the latest version.",
    .updatesNone = "No version has been published yet.",
    .updatesOffline = "GitHub cannot be reached right now.",
    .checkConnection = "Check connection",
    .connectionTitle = "Connection to {0}",
    .connectionMeasuring = "Measuring",
    .connectionMeasuringHint = "VeloPlay sends the console small packets and measures how fast and how evenly it answers.",
    .qualityExcellent = "Excellent",
    .qualityGood = "Good",
    .qualityFair = "Fair",
    .qualityPoor = "Poor",
    .qualityExcellentHint = "As steady as a cable. High picture quality is no problem.",
    .qualityGoodHint = "Good for playing. Response times hardly vary.",
    .qualityFairHint = "Playable, with occasional stutter or short input delay.",
    .qualityPoorHint = "Expect stutter and noticeable input delay here.",
    .connectionNoAnswer = "The console does not answer.",
    .statResponse = "Response time",
    .statJitter = "Variation",
    .statLoss = "Loss",
    .msValue = "{0} ms",
    .percentValue = "{0} %",
    .connectionGraphHint = "Each bar is one measurement. Bars of equal height are good, red marks are lost packets.",
    .adviceTitle = "Recommended setting",
    .adviceText = "{0} Mbit/s, network buffer \"{1}\"",
    .adviceApply = "Apply",
    .adviceApplied = "Applied",
    .tipsTitle = "What makes the link steadier",
    .tipBand = "Set a fixed Wi-Fi band on the PS5 and try both: 5 GHz is faster, 2.4 GHz reaches further. Measure again after each change.",
    .tipBridge = "Put a Wi-Fi repeater or mesh node with a LAN port next to the PS5 and connect the PS5 with a short cable.",
    .tipPowerline = "Powerline adapters carry the network over the power line. Across several floors that is often steadier than Wi-Fi.",
    .tipQuiet = "Pause large downloads on the PS5 and on other devices while playing.",
    .hudConsole = "PS5",
    .hudDisplay = "Display",
    .hudFrameGeneration = "Frame generation",
    .hudPaused = "paused",
    .hudBitrate = "Bitrate",
    .hudResponse = "Response",
    .hudLoss = "Loss",
    .hudAddedDelay = "Added delay",
    .hudFrameTimes = "Frame times",
    .fpsValue = "{0} fps",

    .noticeWakeTimedOut = "{0} did not wake up. On the PS5, open Settings > System > Power Saving > Features Available "
                          "in Rest Mode and check that \"Stay Connected to the Internet\" and \"Enable Turning On PS5 "
                          "from Network\" are on.",
    .noticeWakeFailed = "{0} cannot be woken because its address is unknown.",
    .noticeConfigSaveFailed = "The settings could not be saved. {0}",
    .noticeConfigLoadFailed = "The settings file is damaged. VeloPlay starts with defaults. {0}",
    .noticeDiscoveryFailed = "The network search could not start. {0}",
    .noticeRemoteApiFailed = "Port {0} is in use. Remote control stays off until you pick another port.",
    .noticeSessionRefused = "{0} does not accept the connection. Check that the PS5 is on and that “Enable Remote "
                            "Play” is switched on under Settings > System > Remote Play.",
    .noticeSessionInUse = "{0} is being used by another Remote Play device. End that connection and try again.",
    .noticeSessionVersionMismatch = "{0} uses a Remote Play version VeloPlay does not know yet. Check for an update "
                                    "in the settings under Advanced.",
    .noticeSessionConsoleCrashed = "Remote Play crashed on {0}. Restart the PS5 and try again.",
    .noticeSessionSetupFailed = "The connection to {0} could not be set up. Check that the PS5 is on and Remote Play "
                                "is enabled. If this keeps happening, pair the console again.",
    .noticeSessionLost = "The connection to {0} broke and could not be restored. “Check connection” in the console's "
                         "menu measures the network.",
    .noticeSleepFailed = "{0} did not receive the rest mode command. The connection was closed anyway.",

    .connecting = "Connecting to {0}",
    .connectingHint = "This usually takes a few seconds.",
    .connectingAttempt = "The console is still starting. Attempt {0} of {1}.",
    .loginPinTitle = "{0} asks for your login passcode",
    .loginPinBody = "This is the four-digit code that protects your user on the PS5. VeloPlay does not store it.",
    .loginPinWrong = "That code was wrong. Try again.",
    .loginPinLabel = "Login passcode",
    .loginPinSubmit = "Sign in",
    .streamingTitle = "Connected to {0}",
    .streamingBody = "The connection is up. The first picture is on its way.",
    .streamingNoController = "No controller is connected. Plug one in to control the PS5.",
    .streamingReceived = "Received: {0} frames, {1} audio packets, {2} MB",
    .streamingLost = "Lost frames: {0}",
    .disconnect = "Disconnect",
    .sleepConsole = "Put PS5 in rest mode",
    .reconnectingTitle = "Connection to {0} interrupted",
    .reconnectingBody = "VeloPlay is reconnecting. Attempt {0} of {1}.",
    .disconnecting = "Disconnecting",
    .adaptiveTriggersHint = "The resistance of L2 and R2 as the game sets it. Needs a DualSense.",
    .streamingNoPicture = "No picture arrives, or your PC cannot decode it. The file veloplay.log in the folder "
                          "%APPDATA%\\VeloPlay names the reason. The controller drives the PS5 all the same.",
    .pauseBody = "The game keeps running and your controller keeps driving the PS5. Use the mouse for these buttons.",
    .resume = "Resume",
    .streamKeys = "Esc: menu  ·  F11: full screen  ·  F9: display",
    .hudDecoder = "Decoder",
    .noticeBitrateLowered = "No picture arrived at the bitrate that was set. VeloPlay reconnected and lowered it to "
                            "{0} Mbit/s for this session. Change it for good under Settings > Stream.",    .accountSignIn = "Sign in with PlayStation",
    .accountSignInHint = "Opens Sony's sign-in page in a window of its own. You sign in there, VeloPlay then reads "
                         "your account ID. VeloPlay neither sees nor stores your password.",
    .accountSigningIn = "Waiting for you to sign in at Sony. If your browser opened for it, copy the address "
                        "of the empty page after signing in and paste it above.",
    .accountFetching = "Asking Sony for your account ID.",
    .accountLookupFailed = "The account ID could not be fetched. Try again or enter it by hand.",
    .hudDropped = "Dropped",
    .hudDecodeSlowest = "Slowest picture",
    .automatic2 = "Auto",
    .frameGenerationAutoOn = "Automatic: the PS5 sends {0} fps, your PC computes the frames in between and shows {1} fps. "
                             "The picture appears about {2} ms later in return. A 60 fps stream is shown as it arrives.",
    .frameGenerationAutoOff = "Automatic: the PS5 sends {0} fps. A game that runs at 60 is shown unchanged. For one "
                              "that runs at 30 your PC computes the frames in between.",
    .inputApplyHint = "The PS5 still sends with the old values. The new ones apply after connecting again. The picture "
                      "pauses for a few seconds while the game keeps running.",
    .inputApply = "Connect again now",
    .pauseOverlay = "Display in the top left corner",
    .hudUnavailable = "not available",
    .hudInsidePc = "Inside the PC",
    .modeAuto = "Automatic",
    .modeAutoHint = "VeloPlay chooses resolution, frame rate and bitrate itself and notices whether your game runs "
                    "at 30 or 60 frames per second.",
    .inputAuto = "Automatic",
    .inputAutoHint = "VeloPlay asks for the best resolution at 60 frames per second. If your game only runs at 30, "
                     "VeloPlay notices and computes the frames in between. The bitrate is remembered per PS5: after "
                     "a session without dropouts the next one gets a little more, after dropouts less.",
    .inputAutoHdr = "HDR is not shown yet and therefore stays off.",
    .frameGenerationCapped = "Your screen shows {0} pictures per second. At {2} fps from the PS5 only {1}x fit in; "
                             "more would only delay the picture. VeloPlay therefore uses {1}x.",
    .hudGame = "Game",
    .alertNoController = "No controller connected",
    .alertConnection = "The connection is stalling",
    .alertLost = "Pictures are getting lost on the way",
    .alertDecoder = "Your PC cannot keep up with unpacking the pictures",
    .alertFewer = "The PS5 is sending fewer pictures right now",
};

} // namespace

Language resolveLanguage(std::string_view setting, std::string_view systemLanguage) {
    if (setting == "de") return Language::German;
    if (setting == "en") return Language::English;
    return systemLanguage.starts_with("de") ? Language::German : Language::English;
}

const Strings& strings(Language language) {
    return language == Language::German ? kGerman : kEnglish;
}

std::span<const char* const> allTexts(const Strings& strings) {
    // Strings holds nothing but `const char*` members, so it can be read as an array.
    static_assert(sizeof(Strings) % sizeof(const char*) == 0);
    return {reinterpret_cast<const char* const*>(&strings), sizeof(Strings) / sizeof(const char*)};
}

std::string format(std::string_view pattern, std::initializer_list<std::string_view> arguments) {
    std::string result;
    result.reserve(pattern.size() + 32);
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] == '{' && i + 2 < pattern.size() && pattern[i + 2] == '}' && pattern[i + 1] >= '0' &&
            pattern[i + 1] <= '9') {
            const std::size_t index = static_cast<std::size_t>(pattern[i + 1] - '0');
            if (index < arguments.size()) {
                result += *(arguments.begin() + index);
                i += 2;
                continue;
            }
        }
        result += pattern[i];
    }
    return result;
}

} // namespace veloplay::ui
