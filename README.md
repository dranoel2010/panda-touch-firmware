# Panda-Touch-Firmware

Eigene Firmware für den BIGTREETECH Panda Touch (ESP32-S3, 5″ 800×480, GT911-Touch),
die einen Bambu Lab P1S direkt per MQTT im LAN steuert (Developer Mode nötig).
Baut auf dem BSP [fmauneko/pandatouch](https://github.com/fmauneko/pandatouch-bsp) (MIT) und LVGL 9 auf.

## Oberfläche „Soft White“
Das Design liegt als klickbarer Prototyp unter `design/soft-white/prototype` (unverändert übernommen).
**Es wird nicht nachgebaut, sondern vermessen:**

1. `tools/design/extract.js` öffnet den Prototyp in Chrome (800×480) und misst jeden Bildschirm und Zustand:
   Position, Farben, Verläufe, Radien, Rahmen, Schatten, Schriften, Ausrichtung, gedrückt/ausgewählt.
2. `tools/design/layers.js` rendert jeden einzelnen `box-shadow` als Alpha-Maske, dazu Icons, Bilder,
   Schieberegler und Pfeile der Auswahllisten.
3. `tools/design/gen.py` erzeugt daraus `main/ui/gen/` (Tabellen je Bildschirm + Bausteine).
   Schriften: `tools/design/make_fonts.py` (Manrope in den exakten Stärken 400/600/650/700, Tabellenziffern).

Auf dem Gerät zeichnet `main/ui/sw_render.c` jede Fläche in der Reihenfolge des Browsers
(äußere Schatten → Verlauf → innere Schatten → Rahmen → Inhalt). Texte setzt `sw_text.c` mit Subpixel-Laufweite
wie Chrome. Verläufe werden mit Dithering auf das RGB565-Display gebracht. Die App-Logik `sw_app.c` ist eine
1:1-Portierung von `app.js` (zurzeit mit den Demo-Daten des Prototyps).

Neu erzeugen (Chrome, Node, Python mit fonttools/numpy/pillow nötig):
```bash
cd tools/design
NODE_PATH=<node_modules mit puppeteer-core> node extract.js && node layers.js && python gen.py
```

## Prüfen
- `tools/sim`: dieselbe Oberfläche als Mac-Programm (`make`, dann `./sim bild.ppm nav:menu`).
- `python tools/sim/regress.py` vergleicht alle Bildschirme Pixel für Pixel mit dem Prototyp,
  `--device` macht das Gleiche auf dem angeschlossenen Gerät (Debug-Kanal, Licht bleibt aus).
- Debug-Kanal über USB (115200 Baud): `snap`, `fb0`, `go <aktion>`, `dark 1`, `perf`, `prof <n>` – siehe `main/sw_debug.c`.

## Bauen
GitHub Actions baut bei jedem Push `firmware.bin` (Artefakt „firmware“). Lokal mit Docker:
```bash
docker run --rm -v "$PWD":/project -w /project espressif/idf:v6.0 idf.py build
```

## Flashen (Mac)
Der Panda Touch hat einen CH340-USB-Chip; nötig ist der WCH-Treiber (CH34xVCPDriver), Port `/dev/cu.wchusbserial*`.
Zuverlässig mit 230400 Baud; am besten direkt am Mac, nicht über einen USB-Hub (Stromeinbrüche).
```bash
python3 -m esptool --chip esp32s3 --port /dev/cu.wchusbserialXXXX --baud 230400 write_flash 0 build/firmware.bin
```
**Vorher einmal die Original-Firmware sichern** (`read_flash 0 0x1000000 pandatouch_original.bin`, in 1-MB-Stücken
mit 230400 Baud). Zurückspielen: `write_flash 0 pandatouch_original.bin`.

## Stand
- v0.1.0: Display + Touch-Test
- v0.1.1: Helligkeitsregler (wird gespeichert)
- v0.2.0: Oberfläche „Soft White“ nach dem Prototyp: alle Bildschirme, Dialoge, Schalter, Regler; Demo-Daten;
  Einstellungen bleiben erhalten; startet dunkel im Ruhebildschirm
