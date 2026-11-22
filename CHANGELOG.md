# Changelog

What changed in each published version. The release workflow copies the section of a version into
its release page, which is what the update dialog in VeloPlay links to. English first, German below.

## 0.18.0

- **Menus of a game with frame generation.** When a game replaces one page of a menu by the next
  while everything around it stays, the computed picture showed scraps of both pages for a
  moment. VeloPlay now looks at the picture in parts: a part that stood still until now and
  then fits the picture before neither moved nor in place was replaced, and shows the new page
  at once. In the test scene the computed picture was 19.7 grey levels away from the new page
  and is 0.97 away now. Not yet confirmed in a game.
- **Mouse and keyboard: a page of its own and two presets.** The settings have a page "Mouse &
  keys". The keys to start with now sit where PC games have them: Space is Cross (jump), Shift
  is L3 (run), Ctrl is Circle, R is Square, F is Triangle, Q and E are L1 and R1. A second
  preset, "Open world", is for games with walking and driving: Tab is L1 (often the weapon
  wheel), Q is R1, C is R3, M is the touchpad, P is Options. One click applies a preset, for all
  games or for one. Until now running was on Cross and jumping on Square, as older games had
  it; a layout nobody changed moves to the new start, a layout you made stays.
- **Rounded corners cut off.** The button "Add console" lost its left side with the rounded
  corners while the pointer was on it. Fixed, and the same for every button of that kind.
- **Settings: the last row faded out wrongly.** Where the list of settings continues below the
  visible part, the row at the edge was cut hard in the middle. It fades out now, as intended.
- **A check for both.** The tool that renders every screen now notes every surface and every
  text that an edge cuts off, also for buttons whose background shows only under the pointer.
  A finding fails the build.

**Deutsch**

- **Menüs eines Spiels mit Zwischenbildern.** Ersetzt ein Spiel eine Menüseite durch die
  nächste, während drumherum alles steht, zeigte das berechnete Bild für einen Moment Fetzen
  beider Seiten. VeloPlay sieht sich das Bild jetzt in Teilen an: Ein Teil, der bis eben
  stillstand und dann weder verschoben noch an Ort und Stelle zum vorigen Bild passt, wurde
  ersetzt und zeigt sofort die neue Seite. In der Testszene lag das berechnete Bild 19,7
  Graustufen von der neuen Seite entfernt, jetzt 0,97. Im Spiel noch nicht bestätigt.
- **Maus und Tastatur: eine eigene Seite und zwei Vorlagen.** Die Einstellungen haben eine Seite
  „Maus & Tastatur“. Die Tasten liegen zu Beginn dort, wo PC-Spiele sie haben: Leertaste ist
  Kreuz (springen), Umschalt ist L3 (sprinten), Strg ist Kreis, R ist Quadrat, F ist Dreieck, Q
  und E sind L1 und R1. Eine zweite Vorlage, „Offene Welt“, ist für Spiele, in denen du läufst
  und fährst: Tab ist L1 (oft das Waffenrad), Q ist R1, C ist R3, M das Touchpad, P ist Options.
  Ein Klick setzt eine Vorlage, für alle Spiele oder für eines. Bisher lag Sprinten auf Kreuz
  und Springen auf Quadrat, wie ältere Spiele es hatten; eine Belegung, die nie geändert wurde,
  wechselt zur neuen, eine eigene bleibt.
- **Abgeschnittene runde Ecken.** Dem Knopf „Konsole hinzufügen“ fehlte die linke Seite mit den
  runden Ecken, solange der Mauszeiger darauf stand. Behoben, auch bei allen Knöpfen dieser Art.
- **Einstellungen: die letzte Zeile blendete falsch aus.** Wo die Liste der Einstellungen
  unterhalb des sichtbaren Teils weitergeht, war die Zeile am Rand mittendrin hart
  abgeschnitten. Jetzt blendet sie weich aus, wie gedacht.
- **Eine Prüfung für beides.** Das Werkzeug, das jeden Bildschirm rendert, merkt sich jetzt jede
  Fläche und jeden Text, den ein Rand abschneidet, auch bei Knöpfen, deren Hintergrund erst
  unter dem Mauszeiger erscheint. Ein Fund lässt den Build scheitern.

## 0.17.1

- **Flicker on coloured things in computed pictures.** In a cutscene a blue nozzle flickered
  badly while frame generation was on. It was exactly as bright as the wall behind it and
  differed only in colour, and the search for motion compared brightness alone: it did not see
  the nozzle, followed the grain of the picture instead and mixed blue and grey. The search now
  compares colour as well. On NVIDIA cards the card's own search still sees brightness only, so
  VeloPlay's search now runs next to it and takes over where it fits clearly better. In a test
  scene built after the report, colour in the computed picture was off by 2.68 before and is off
  by 0.19 now; a plain mix of the two pictures gives 1.21. Not yet confirmed in the game.

**Deutsch**

- **Flackern an farbigen Dingen in berechneten Bildern.** In einer Zwischensequenz flackerte eine
  blaue Zapfpistole stark, solange die Zwischenbilder an waren. Sie war genau so hell wie die
  Wand dahinter und unterschied sich nur in der Farbe, und die Bewegungssuche verglich nur die
  Helligkeit: Sie sah die Zapfpistole nicht, folgte stattdessen dem Bildrauschen und mischte Blau
  und Grau durcheinander. Die Suche vergleicht jetzt auch die Farbe. Auf NVIDIA-Karten sieht
  deren eigene Suche weiter nur Helligkeit, deshalb läuft die Suche von VeloPlay jetzt daneben
  mit und übernimmt, wo sie deutlich besser passt. In einer Testszene, die nach der Meldung
  gebaut wurde, lag die Farbe im berechneten Bild vorher um 2,68 daneben und jetzt um 0,19; ein
  schlichtes Überblenden der beiden Bilder liegt bei 1,21. Im Spiel noch nicht bestätigt.

## 0.17.0

- **A window of its own.** VeloPlay no longer has the title bar of Windows. At the top it draws
  its own bar: name, the time, and buttons for full screen, minimise, enlarge and close. The
  window is dragged by that bar and resized at its edges as before, and snapping to the sides
  of the screen still works.
- **Starts in full screen.** VeloPlay now opens over the whole screen. F11 or the button in the
  bar switches to a window, and it remembers which you chose. In full screen the taskbar of
  Windows stays out of the way and comes forward when the pointer rests on the lower edge of
  the screen for a moment; it leaves again when the pointer does.

**Deutsch**

- **Ein eigenes Fenster.** VeloPlay hat nicht mehr die Titelleiste von Windows. Oben zeichnet es
  eine eigene Leiste: Name, Uhrzeit und Knöpfe für Vollbild, Minimieren, Vergrößern und
  Schließen. An dieser Leiste verschiebst du das Fenster, an den Rändern änderst du wie bisher
  die Größe, und das Andocken an die Bildschirmseiten geht weiter.
- **Startet im Vollbild.** VeloPlay öffnet sich jetzt über den ganzen Bildschirm. F11 oder der
  Knopf in der Leiste wechselt zum Fenster, und VeloPlay merkt sich deine Wahl. Im Vollbild
  bleibt die Taskleiste von Windows aus dem Weg und kommt hervor, wenn der Mauszeiger einen
  Moment am unteren Bildschirmrand ruht; sie verschwindet wieder, wenn er weg ist.

## 0.16.0

- **VeloPlay can be steered with the controller.** On the start page and in the settings the
  directional pad or the left stick moves a mark from button to button, Cross selects, Circle
  goes back, Options opens the settings, and L1 and R1 turn their pages. Until now only the
  sliders reacted; the buttons were not part of it. In the game the controller belongs to the
  PS5, as before.
- **A page to try the controller**, under Settings > Controller. A drawing of the controller
  shows live what it reports: every button, both sticks, how far L2 and R2 are pressed, and
  how often it reports. One button makes it vibrate. On a DualSense three effects for the
  adaptive triggers can be switched on without a console: resistance over nearly the whole
  travel at full force, a pressure point as on a weapon's trigger, and shaking. That shows what
  your controller can do, to compare with what arrives in a game.

**Deutsch**

- **VeloPlay lässt sich mit dem Controller bedienen.** Auf der Startseite und in den
  Einstellungen bewegt das Steuerkreuz oder der linke Stick eine Markierung von Knopf zu Knopf,
  Kreuz wählt aus, Kreis geht zurück, Options öffnet die Einstellungen, L1 und R1 blättern
  darin. Bisher reagierten nur die Schieberegler; die Knöpfe waren nicht dabei. Im Spiel gehört
  der Controller wie bisher der PS5.
- **Eine Seite zum Testen des Controllers**, unter Einstellungen > Controller. Eine Zeichnung
  des Controllers zeigt live, was er meldet: jeden Knopf, beide Sticks, wie weit L2 und R2
  gedrückt sind, und wie oft er meldet. Ein Knopf lässt ihn vibrieren. An einem DualSense
  lassen sich drei Effekte der adaptiven Trigger ohne Konsole einschalten: Widerstand fast über
  den ganzen Weg mit voller Kraft, ein Druckpunkt wie am Abzug einer Waffe, und Rütteln. Das
  zeigt, was dein Controller kann, zum Vergleich mit dem, was im Spiel ankommt.

## 0.15.1

- **Frame generation: ladders and fences are right again.** 0.15.0 held every spot in place that
  looked the same in two pictures in a row, to stop buttons of the game from flickering. That
  also held the gaps between the rungs of a ladder the camera moves along, where the rung
  belongs in between; a measurement on a real card showed it right after the release. That rule
  is gone. Instead, something that stands still is now held after about a third of a second
  rather than a full second, which shortens the flicker of a button that has just appeared
  without touching anything that moves.

**Deutsch**

- **Zwischenbilder: Leitern und Zäune stimmen wieder.** 0.15.0 hielt jede Stelle fest, die in
  zwei Bildern nacheinander gleich aussah, damit Knöpfe des Spiels nicht flackern. Damit wurden
  auch die Lücken zwischen den Sprossen einer Leiter festgehalten, an der die Kamera
  entlangfährt, und genau dort gehört die Sprosse im Zwischenbild hin; eine Messung auf einer
  echten Grafikkarte hat das gleich nach der Veröffentlichung gezeigt. Diese Regel ist wieder
  draußen. Stattdessen wird Stillstehendes jetzt nach etwa einer Drittelsekunde festgehalten
  statt nach einer ganzen. Das verkürzt das Flackern eines gerade eingeblendeten Knopfs, ohne
  etwas anzufassen, das sich bewegt.

## 0.15.0

- **Microphone for voice chat.** New under Settings > Advanced and in the menu behind Esc, off
  until you switch it on. While you are connected, the sound of your default microphone goes
  to the PS5, so the others in a party hear you. The top right corner of the picture says
  “Microphone on” for as long as that is the case. If Windows switches to another microphone
  or yours is plugged in later, VeloPlay follows. Use headphones, or the others hear the game's
  sound as well. New and not yet tried with a real party.
- **A small display.** F9 now steps through three: off, a small one with pictures per second,
  response time and bitrate on one line, and the one with all figures. The menu behind Esc and
  the settings offer the same choice.
- **Less flicker around the game's own display.** With frame generation, a button or a number
  the game had just put up over a moving scene was dragged along with the scene for about a
  second, which showed as a flicker. What is the same in two pictures in a row now stays where
  it is from the first moment. The grounds of VeloPlay's own displays are solid now instead of
  see-through, and the menu darkens the picture more, so nothing flickers behind them.

**Deutsch**

- **Mikrofon für den Sprachchat.** Neu unter Einstellungen > Erweitert und im Menü hinter Esc,
  aus, bis du es einschaltest. Solange du verbunden bist, geht der Ton deines Standard-Mikrofons
  an die PS5, die anderen in einer Party hören dich also. Oben rechts im Bild steht „Mikrofon
  an“, solange das so ist. Wechselt Windows auf ein anderes Mikrofon oder steckst du deines
  später ein, zieht VeloPlay mit. Nimm Kopfhörer, sonst hören die anderen den Ton des Spiels
  mit. Neu und noch mit keiner echten Party ausprobiert.
- **Eine kleine Anzeige.** F9 schaltet jetzt durch drei Stufen: aus, eine kleine mit Bildern pro
  Sekunde, Antwortzeit und Datenrate in einer Zeile, und die mit allen Werten. Das Menü hinter
  Esc und die Einstellungen bieten dieselbe Auswahl.
- **Weniger Flackern an der Anzeige des Spiels.** Mit Zwischenbildern wurde ein Knopf oder eine
  Zahl, die das Spiel gerade über eine bewegte Szene gelegt hatte, etwa eine Sekunde lang mit
  der Szene mitgezogen; das sah man als Flackern. Was in zwei Bildern nacheinander gleich ist,
  bleibt jetzt vom ersten Moment an, wo es ist. Die Hintergründe von VeloPlays eigenen Anzeigen
  sind jetzt deckend statt durchscheinend, und das Menü dunkelt das Bild stärker ab, damit
  dahinter nichts mehr flackert.

## 0.14.0

- **A new menu in the game.** Esc now shows at a glance how picture, connection and controls are
  doing: three tiles with pictures per second, a verdict on the link with bitrate, response
  time and loss, and the controller with its battery. A click on a tile opens its settings.
  Frame generation, upscaling, the display in the corner and keys and mouse can be switched
  right there. Disconnect and rest mode sit apart on the right.
- **Key layouts per game.** With keys and mouse, a game can have a layout of its own, with its
  own mouse sensitivity. It applies by itself as soon as that game runs on the PS5; all others
  use the general layout. F1 shows the layout in use over the picture, for the moment the game
  says “press Cross” and you wonder which key that was.
- **The controller's battery** is shown next to its name and in the menu. A note appears over
  the picture at 15 and at 5 percent.
- **Sound follows your headphones.** When Windows moves its default output while you play
  (headphones plugged in, a headset switched on) or the device goes away, the sound moves with
  it. Until now it stayed silent until the next connection.

**Deutsch**

- **Ein neues Menü im Spiel.** Esc zeigt jetzt auf einen Blick, wie es Bild, Verbindung und
  Steuerung geht: drei Kacheln mit Bildern pro Sekunde, einem Urteil über die Verbindung samt
  Datenrate, Antwortzeit und Verlust, und dem Controller mit Akkustand. Ein Klick auf eine
  Kachel öffnet die passenden Einstellungen. Zwischenbilder, Hochskalierung, die Anzeige in
  der Ecke und Maus und Tastatur lassen sich gleich dort umschalten. Trennen und Ruhemodus
  stehen abgesetzt rechts.
- **Tastenbelegung pro Spiel.** Mit Maus und Tastatur kann ein Spiel eine eigene Belegung
  bekommen, mit eigener Mausempfindlichkeit. Sie gilt von selbst, sobald das Spiel auf der PS5
  läuft; für alle anderen gilt die allgemeine. F1 blendet die geltende Belegung über dem Bild
  ein, für den Moment, in dem das Spiel „Drücke Kreuz“ sagt und du überlegst, welche Taste das
  war.
- **Der Akku des Controllers** steht neben seinem Namen und im Menü. Bei 15 und bei 5 Prozent
  erscheint ein Hinweis über dem Bild.
- **Der Ton folgt deinen Kopfhörern.** Wechselt Windows beim Spielen das Standard-Tongerät
  (Kopfhörer eingesteckt, Headset eingeschaltet) oder verschwindet das Gerät, zieht der Ton
  mit um. Bisher blieb er bis zur nächsten Verbindung stumm.

## 0.13.0

- **Compare the upscaling while you play: F8.** During a stream F8 steps through off, Lanczos
  and, on NVIDIA cards, RTX Video. A note at the bottom names what is active for four seconds.
  It also says when a step cannot change anything, because the window is no larger than the
  stream.
- **RTX Video now also works when the picture keeps its size.** Until now it only ran when the
  picture was enlarged. With a 1080p stream on a 1080p screen that never happened, so switching
  it on made no difference. A new test on a real card sends the same picture through NVIDIA's
  video processor with and without its network. On an RTX 4070 the network changed close to
  half of the picture's points even at the same size: it also smooths what the video
  compression left. It costs the graphics card a few milliseconds per picture; F8 or the
  settings switch it off. Whether the picture looks better is for your eyes to judge.

**Deutsch**

- **Hochskalierung im Spiel vergleichen: F8.** Im laufenden Stream schaltet F8 zwischen Aus,
  Lanczos und, auf NVIDIA-Karten, RTX Video um. Ein Hinweis unten nennt für vier Sekunden, was
  gerade gilt. Er sagt auch, wenn eine Stufe nichts ändern kann, weil das Fenster nicht größer
  ist als der Stream.
- **RTX Video wirkt jetzt auch, wenn das Bild seine Größe behält.** Bisher lief es nur, wenn
  das Bild vergrößert wurde. Bei einem 1080p-Stream auf einem 1080p-Bildschirm kam das nie
  vor, das Einschalten änderte also nichts. Ein neuer Test auf einer echten Karte schickt
  dasselbe Bild mit und ohne NVIDIAs Netz durch den Videoprozessor. Auf einer RTX 4070
  veränderte das Netz auch bei gleicher Größe knapp die Hälfte der Bildpunkte: Es glättet auch,
  was die Videokompression hinterlassen hat. Das kostet die Grafikkarte einige Millisekunden
  pro Bild; mit F8 oder in den Einstellungen schaltest du es ab. Ob das Bild besser aussieht,
  entscheiden deine Augen.

## 0.12.0

- **Playing with mouse and keyboard.** New under Settings > Controller, off by default. Keys
  press the buttons and push the left stick, the mouse moves the right stick: the faster the
  hand, the further the stick. The layout that comes with it suits games seen from behind the
  character (W A S D to walk, mouse to look, left button to shoot, right button to aim) and can
  be changed: click a part of the drawn controller or its field, then press the key. While the
  game is shown, mouse and keyboard belong to the console; Esc opens the menu and frees them.
  The PS5 keeps showing controller symbols, since a controller is all it sees. A mouse through a
  stick never feels quite like a mouse in a PC game; raising the camera sensitivity in the game
  helps. New and not yet played with on a real console.

**Deutsch**

- **Spielen mit Maus und Tastatur.** Neu unter Einstellungen > Controller, zunächst aus. Tasten
  drücken die Knöpfe und bewegen den linken Stick, die Maus bewegt den rechten: je schneller
  die Hand, desto weiter der Stick. Die mitgelieferte Belegung passt zu Spielen mit Blick von
  hinten auf die Figur (W A S D zum Laufen, Maus zum Umsehen, linke Taste schießen, rechte
  zielen) und lässt sich ändern: einen Teil des gezeichneten Controllers oder sein Feld
  anklicken, dann die Taste drücken. Solange das Spiel zu sehen ist, gehören Maus und Tastatur
  der Konsole; Esc öffnet das Menü und gibt sie frei. Die PS5 zeigt weiter Controller-Symbole,
  denn sie sieht nur einen Controller. Eine Maus über einen Stick fühlt sich nie ganz an wie
  eine Maus in einem PC-Spiel; die Kamera-Empfindlichkeit im Spiel hochzustellen hilft. Neu und
  noch an keiner echten Konsole gespielt.

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
