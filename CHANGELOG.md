# Changelog

What changed in each published version. The release workflow copies the section of a version into
its release page, which is what the update dialog in VeloPlay links to. English first, German below.

## 0.11.0

- **Cuts no longer flash.** When the game jumps to another picture, a computed picture used to
  show both scenes at once for a moment. If most of the picture finds no counterpart in the next
  one, the nearer real picture is shown instead.
- **The game's own display stays calm.** A map, a counter or writing stands still while the scene
  moves behind it, and its edges could shimmer in computed pictures. Edges that have stood still
  for about a second are now taken from the real pictures as they are.
- **Adaptive triggers and haptics at the strength the console asks for.** The PS5 says how strong
  the triggers and the vibration are to be (its setting under Accessories > Controllers).
  VeloPlay did not pass that on, so the controller kept whatever strength the program before had
  left it at. It is now set to full strength when a session starts and follows the console from
  there. This comes from a report that the resistance of the triggers is there but weaker than at
  the console; whether it is the whole reason is open.

**Deutsch**

- **Schnitte blitzen nicht mehr.** Springt das Spiel zu einem anderen Bild, zeigte ein
  berechnetes Bild bisher für einen Moment beide Szenen gleichzeitig. Findet der größte Teil
  des Bildes im nächsten kein Gegenstück, wird stattdessen das nähere echte Bild gezeigt.
- **Die Anzeige des Spiels bleibt ruhig.** Eine Karte, ein Zähler oder Schrift steht still,
  während sich die Szene dahinter bewegt, und ihre Kanten konnten in berechneten Bildern
  flimmern. Kanten, die seit etwa einer Sekunde stillstehen, kommen jetzt unverändert aus den
  echten Bildern.
- **Adaptive Trigger und Haptik in der Stärke, die die Konsole vorgibt.** Die PS5 teilt mit,
  wie stark Trigger und Vibration sein sollen (ihre Einstellung unter Zubehör > Controller).
  VeloPlay gab das nicht weiter, der Controller behielt also die Stärke, die das Programm davor
  hinterlassen hatte. Jetzt wird er beim Start einer Sitzung auf volle Stärke gestellt und folgt
  danach der Konsole. Anlass ist die Rückmeldung, dass der Widerstand der Trigger da ist, aber
  schwächer als an der Konsole; ob das der ganze Grund ist, ist offen.

## 0.10.1

- **"Game 30 fps" no longer drops out while the camera moves.** With every turn the detection
  switched off and on again, and frame generation with it. A repeated picture is not identical
  to the one before: the encoder uses it to touch the picture up, the more the faster it moves.
  The detection now judges the rhythm (every other picture changes clearly less than its
  neighbours) instead of a fixed limit, and only changes its mind after more than half a second
  of evidence.
- **Connecting again right after a session waits for the console.** After "Connect again now",
  or a quick disconnect and connect, the PS5 still counted the old session as running and
  answered "in use", which ended in an error. VeloPlay now keeps trying with the waiting screen
  until the console lets go, for up to 75 seconds.

**Deutsch**

- **„Spiel 30 fps“ fällt bei Kamerabewegung nicht mehr aus.** Bei jeder Drehung schaltete die
  Erkennung ab und wieder an, und die Zwischenbilder mit ihr. Ein wiederholtes Bild ist nicht
  gleich dem davor: Der Encoder bessert darin nach, umso mehr, je schneller sich das Bild
  bewegt. Die Erkennung beurteilt jetzt den Rhythmus (jedes zweite Bild ändert sich deutlich
  weniger als seine Nachbarn) statt einer festen Grenze und ändert ihre Meinung erst nach mehr
  als einer halben Sekunde.
- **Neuverbinden direkt nach einer Sitzung wartet auf die Konsole.** Nach „Jetzt neu
  verbinden“ oder nach schnellem Trennen und Verbinden zählte die PS5 die alte Sitzung noch als
  laufend und meldete „wird schon genutzt“, was in einer Fehlermeldung endete. VeloPlay
  versucht es jetzt mit dem Wartebildschirm weiter, bis die Konsole freigibt, höchstens 75
  Sekunden.

## 0.10.0

- **Notes over the running picture.** VeloPlay now says what is wrong while it is wrong: no
  controller connected, the connection stalls, pictures get lost on the way, the PC cannot keep
  up with unpacking, or the PS5 sends fewer pictures than asked for. A note appears at the top of
  the picture and goes a few seconds after the cause is gone. Each one is also written to the
  log.
- **Frame generation with NVIDIA Optical Flow.** On NVIDIA cards from the RTX 20 series on, the
  motion between two pictures is now found by the card's own engine for that, which is part of
  the driver. It copes with things the built-in search got wrong, such as the rungs of a ladder
  or a fence while the camera moves. The display names the method ("NVIDIA Optical Flow" or
  "VeloPlay Flow"). On other cards, and whenever the engine is missing or reports an error, the
  built-in search keeps working as before. Nothing has to be set. New, and when this version was
  made it had not yet run on a real NVIDIA card: if pictures in between look worse than before,
  that is worth a report.

**Deutsch**

- **Hinweise über dem laufenden Bild.** VeloPlay sagt jetzt, was nicht stimmt, solange es nicht
  stimmt: kein Controller verbunden, die Verbindung hakt, Bilder gehen unterwegs verloren, der PC
  kommt mit dem Entpacken nicht nach, oder die PS5 schickt weniger Bilder als verlangt. Der
  Hinweis erscheint oben im Bild und verschwindet ein paar Sekunden, nachdem die Ursache weg ist.
  Jeder Hinweis steht auch im Protokoll.
- **Zwischenbilder mit NVIDIA Optical Flow.** Auf NVIDIA-Karten ab der RTX-20-Serie sucht jetzt
  der dafür gebaute Teil der Grafikkarte die Bewegung zwischen zwei Bildern; er gehört zum
  Treiber. Er kommt mit Dingen zurecht, bei denen die eigene Suche danebenlag, etwa den Sprossen
  einer Leiter oder einem Zaun, während sich die Kamera bewegt. Die Anzeige nennt das Verfahren
  („NVIDIA Optical Flow“ oder „VeloPlay Flow“). Auf anderen Karten, und immer wenn der Teil fehlt
  oder einen Fehler meldet, arbeitet die eigene Suche wie bisher. Einstellen muss man nichts.
  Neu, und beim Bau dieser Version noch auf keiner echten NVIDIA-Karte gelaufen: Sehen
  Zwischenbilder schlechter aus als vorher, ist das eine Meldung wert.

## 0.9.0

The input sets itself:

- **"Automatic" is the new default.** VeloPlay asks the console for 1080p at 60 frames per second
  with HEVC and chooses the bitrate itself. Settings > Input has one switch for it; the manual
  choices are behind it and keep their values. Existing installations switch to it once.
- **A game that runs at 30 fps is recognised.** The console then sends every picture twice.
  VeloPlay compares each picture with the one before, leaves the repeats out and computes the
  pictures in between as before. The display shows "Game 30 fps" while that is the case. New and
  not yet seen on a real game: if motion looks choppy, pick "30 fps game" on the start screen.
- **The bitrate is remembered per console.** It starts at 15 Mbit/s. After a session without
  dropouts the next one gets a quarter more, up to 30 Mbit/s; after lost pictures or a stream
  that stood still it gets less, and VeloPlay remembers where the trouble began.
- **Frame generation respects the screen.** 3x and 4x on a 60 Hz screen only added delay. The
  factor now stays within what the screen can show, and the settings say so.

**Deutsch**

Der Eingang stellt sich selbst ein:

- **„Automatisch“ ist die neue Voreinstellung.** VeloPlay fordert von der Konsole 1080p mit
  60 Bildern pro Sekunde und HEVC an und wählt die Datenrate selbst. Unter Einstellungen >
  Eingang gibt es dafür einen Schalter; die Auswahl von Hand liegt dahinter und behält ihre
  Werte. Bestehende Installationen wechseln einmal dorthin.
- **Ein Spiel, das mit 30 Bildern läuft, wird erkannt.** Die Konsole schickt dann jedes Bild
  doppelt. VeloPlay vergleicht jedes Bild mit dem davor, lässt die Wiederholungen weg und
  berechnet die Bilder dazwischen wie bisher. Die Anzeige nennt dann „Spiel 30 fps“. Neu und noch
  an keinem echten Spiel gesehen: Wirkt Bewegung abgehackt, auf der Startseite „30-fps-Spiel“
  wählen.
- **Die Datenrate merkt sich VeloPlay pro Konsole.** Sie beginnt bei 15 Mbit/s. Nach einer
  Sitzung ohne Aussetzer gibt es beim nächsten Mal ein Viertel mehr, bis 30 Mbit/s; nach
  verlorenen Bildern oder einem stehenden Stream weniger, und VeloPlay merkt sich, wo der Ärger
  anfing.
- **Zwischenbilder richten sich nach dem Bildschirm.** 3x und 4x auf einem Bildschirm mit 60 Hz
  brachten nur Verzögerung. Der Faktor bleibt jetzt bei dem, was der Bildschirm zeigen kann, und
  die Einstellungen sagen das.

## 0.8.0

Against the delay between pressing a button and seeing the result:

- **The controller is read 500 times a second** instead of once per picture. A press used to
  wait up to 17 ms before it was even sent to the console.
- **The display shows how long a picture spends in the PC** ("Inside the PC"): from its data
  arriving to being drawn. The number also goes into the log every ten seconds.
- **No picture is lost when two arrive right behind each other.** Only the newer one used to be
  kept, and the picture computed in between then bridged twice the motion.

**Deutsch**

Gegen die Verzögerung zwischen Tastendruck und Bild:

- **Der Controller wird 500-mal pro Sekunde gelesen** statt einmal pro Bild. Ein Tastendruck
  wartete bisher bis zu 17 ms, bevor er überhaupt zur Konsole ging.
- **Die Anzeige nennt, wie lange ein Bild im PC unterwegs ist** („Im PC“): vom Eintreffen der
  Daten bis zum Zeichnen. Die Zahl steht auch alle zehn Sekunden im Protokoll.
- **Kein Bild geht mehr verloren, wenn zwei direkt hintereinander ankommen.** Bisher blieb nur
  das neuere, und das berechnete Zwischenbild überbrückte dann die doppelte Bewegung.

## 0.7.1

Two changes against frame generation looking "a bit pixelated and a bit laggy":

- **Computed pictures are as sharp as the real ones.** They used to be slightly softer, which
  showed as a flicker in sharpness between every two pictures.
- **Pictures are shown at even distances.** Wi-Fi delivers unevenly: one picture late, the next
  right behind it. The display followed that rhythm; now each picture gets its full time.

**Deutsch**

Zwei Änderungen dagegen, dass Zwischenbilder „etwas pixelig und etwas laggy“ aussahen:

- **Berechnete Bilder sind so scharf wie die echten.** Sie waren bisher etwas weicher, was als
  Flimmern in der Schärfe zwischen je zwei Bildern auffiel.
- **Bilder werden in gleichmäßigen Abständen gezeigt.** Ein WLAN liefert ungleichmäßig: ein Bild
  verspätet, das nächste direkt dahinter. Die Anzeige folgte diesem Takt; jetzt bekommt jedes
  Bild seine volle Zeit.

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
