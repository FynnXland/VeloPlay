# Changelog

What changed in each published version. The release workflow copies the section of a version into
its release page, which is what the update dialog in VeloPlay links to. English first, German below.

## 0.7.0

- **RTX Video Super Resolution.** On NVIDIA cards the output settings offer a third way to
  enlarge the picture: NVIDIA's neural network, which also smooths compression blocks. It needs
  RTX Video Enhancement with Super Resolution switched on in the NVIDIA app. VeloPlay cannot tell
  whether the driver applies it; the NVIDIA app shows that. The display names the method in use.
- **Updates come from a new place.** Releases are published at github.com/FynnXland/VeloPlay from
  now on, and this version looks there. Coming from 0.6.1 or older, install this version by hand
  once; after that the built-in update works.

**Deutsch**

- **RTX Video Super Resolution.** Auf NVIDIA-Karten gibt es in den Einstellungen unter „Ausgabe“
  einen dritten Weg, das Bild zu vergrößern: das neuronale Netz von NVIDIA, das dabei auch
  Klötzchen aus der Übertragung glättet. Dafür muss in der NVIDIA-App die RTX-Video-Verbesserung
  mit Superauflösung eingeschaltet sein. VeloPlay kann nicht erkennen, ob der Treiber sie
  anwendet; die NVIDIA-App zeigt es an. Die Anzeige nennt das Verfahren, das gerade läuft.
- **Updates kommen von einem neuen Ort.** Versionen erscheinen ab jetzt unter
  github.com/FynnXland/VeloPlay, und diese Version sieht dort nach. Wer von 0.6.1 oder älter
  kommt, installiert diese Version einmal von Hand; danach funktioniert das eingebaute Update.

## 0.6.1

- **Frame generation now reaches the full rate.** With 0.6.0 a 30 fps stream came out at 44 to 50
  pictures a second instead of 60. Each picture was due exactly at a refresh of the screen, and
  when it missed that by a hair it was shown twice and the next one was skipped. Pictures now go
  out with the refresh nearest to their moment.
- **The display shows the size of the picture on screen** next to "Screen", as it already did for
  what the PS5 sends.

**Deutsch**

- **Zwischenbilder erreichen jetzt die volle Rate.** Mit 0.6.0 kam ein Stream mit 30 fps mit 44
  bis 50 Bildern pro Sekunde heraus statt mit 60. Jedes Bild war genau zu einem Bildwechsel des
  Monitors fällig; verpasste es den um Haaresbreite, wurde es doppelt gezeigt und das nächste
  ausgelassen. Jetzt geht jedes Bild mit dem Bildwechsel hinaus, der seinem Zeitpunkt am nächsten
  liegt.
- **Die Anzeige nennt die Größe des Bildes auf dem Bildschirm** neben „Bildschirm“, so wie schon
  bei dem, was die PS5 sendet.

## 0.6.0

The first version with frame generation and upscaling. Both were computed and checked on a
software renderer only; on a real graphics card this version is the first try. If the picture
looks wrong, set frame generation to off under Settings > Output and please report it.

- **Frame generation.** From a 30 fps stream VeloPlay computes the pictures in between and shows
  60. The built-in method ("VeloPlay Flow") follows how things move between two pictures and runs
  on any Direct3D 11 card. The picture appears about 17 ms later in return, and the sound waits
  with it. "Auto" is the default: 30 fps is doubled, 60 fps is shown as it arrives, and which of
  the two arrives is measured. 2x, 3x and 4x can be set by hand.
- **Upscaling and sharpening.** A Lanczos filter enlarges the picture to the window, and the
  sharpness slider now works (after AMD's RCAS).
- **Less delay on screen.** A picture that arrives while VeloPlay waits for the monitor now goes
  out with that refresh instead of the next one.
- **Settings: "Input" and "Output".** What the PS5 sends and what your PC makes of it are two
  tabs with a strip above that shows both. Each says when its changes apply.
- **Settings inside the game.** Esc opens the menu; there you can switch the display in the
  corner on and off and open the settings behind the gear. Changed input values apply with
  "Connect again now".
- **A logo and a program icon.**
- **Hangs after switching back with Alt+Tab (issue 3):** not fixed yet. A second watch now asks
  Windows itself whether the window hangs and logs what explains it.

**Deutsch**

Die erste Version mit Zwischenbildern und Hochskalierung. Beides ist bisher nur auf einem
Software-Renderer gerechnet und geprüft; auf einer echten Grafikkarte ist diese Version der erste
Versuch. Sieht das Bild falsch aus, stell Zwischenbilder unter Einstellungen > Ausgabe auf Aus
und melde es bitte.

- **Zwischenbilder.** Aus einem Stream mit 30 fps berechnet VeloPlay die Bilder dazwischen und
  zeigt 60. Das eingebaute Verfahren („VeloPlay Flow“) verfolgt, wie sich das Bild zwischen zwei
  Bildern bewegt, und läuft auf jeder Direct3D-11-Karte. Das Bild erscheint dafür etwa 17 ms
  später, der Ton wartet mit. Voreingestellt ist „Auto“: 30 fps werden verdoppelt, 60 fps gezeigt,
  wie sie ankommen; was ankommt, wird gemessen. 2x, 3x und 4x lassen sich von Hand wählen.
- **Hochskalieren und Schärfen.** Ein Lanczos-Filter vergrößert das Bild auf das Fenster, und
  der Regler „Schärfe“ wirkt jetzt (nach AMDs RCAS).
- **Weniger Verzögerung am Bildschirm.** Ein Bild, das ankommt, während VeloPlay auf den Monitor
  wartet, geht jetzt mit diesem Bildwechsel hinaus und nicht erst mit dem nächsten.
- **Einstellungen: „Eingang“ und „Ausgabe“.** Was die PS5 schickt und was dein PC daraus macht,
  sind zwei Reiter mit einer Zeile darüber, die beides zeigt. Jeder sagt, wann Änderungen gelten.
- **Einstellungen im Spiel.** Esc öffnet das Menü; dort schaltest du die Anzeige in der Ecke ein
  und aus und öffnest über das Zahnrad die Einstellungen. Geänderte Eingangswerte gelten mit
  „Jetzt neu verbinden“.
- **Ein Logo und ein Programm-Icon.**
- **Hänger nach dem Zurückwechseln mit Alt+Tab (Issue 3):** noch nicht behoben. Eine zweite Wache
  fragt jetzt Windows selbst, ob das Fenster hängt, und schreibt ins Protokoll, was es erklärt.

## 0.5.2

First published version. Finding the console, pairing, connecting and controller input are
confirmed on a real PS5. Picture and sound arrive but are still being worked on (issue 2), and
the fixes below have not been confirmed on a console yet. Please report what you see.

- **The log survives a restart.** `veloplay.log` is the current run; the three runs before it are
  kept as `veloplay.1.log` to `veloplay.3.log` in `%APPDATA%\VeloPlay`. After a hang, the log you
  need is `veloplay.1.log`.
- **A hanging window explains itself.** When the window stops answering for 1.5 seconds, the log
  names the step it was in, says whether the window was minimised, and lists where the thread
  waits (library and function).
- **Slowest picture in the overlay (F9).** Next to the average decoding time there is now the
  slowest picture of the last seconds. That is the number that explains stutter.
- **A line about the stream every ten seconds in the log:** frame rates, bitrate, loss, decoding
  times, dropped frames.
- From 0.5.1: the decoder queue no longer drops frames without telling the console, the display
  no longer holds up the decoder, and the audio reserve is larger for consoles on Wi-Fi.

**Deutsch**

Erste veröffentlichte Version. Suche, Kopplung, Verbindung und Controller-Eingaben sind an einer
echten PS5 bestätigt. Bild und Ton kommen an, daran wird aber noch gearbeitet (Issue 2), und die
Korrekturen unten sind an der Konsole noch nicht bestätigt. Melde bitte, was du siehst.

- **Das Protokoll übersteht einen Neustart.** `veloplay.log` gehört zum laufenden Start, die drei
  Starts davor bleiben als `veloplay.1.log` bis `veloplay.3.log` in `%APPDATA%\VeloPlay`. Nach
  einem Hänger brauchst du `veloplay.1.log`.
- **Ein hängendes Fenster erklärt sich.** Antwortet das Fenster 1,5 Sekunden nicht, steht im
  Protokoll der Schritt, ob das Fenster minimiert war und wo der Thread wartet (Bibliothek und
  Funktion).
- **Langsamstes Bild in der Anzeige (F9).** Neben der mittleren Dekodierzeit steht jetzt das
  langsamste Bild der letzten Sekunden. Diese Zahl erklärt Ruckler.
- **Alle zehn Sekunden eine Zeile zum Stream im Protokoll:** Bildraten, Datenrate, Verlust,
  Dekodierzeiten, verworfene Bilder.
- Aus 0.5.1: Die Warteschlange vor dem Dekoder verwirft keine Bilder mehr, ohne es der Konsole zu
  sagen, die Bildausgabe hält den Dekoder nicht mehr auf, und die Tonreserve ist für Konsolen im
  WLAN größer.
