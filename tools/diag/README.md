# Mess-Build: kommt der Bildnachschub pünktlich?

Das Display holt sein Bild alle 0,9 ms in 20-Zeilen-Stücken aus dem PSRAM (Bounce-Puffer). Kommt ein Stück
zu spät, zuckt oder springt das Bild für einen Moment. Von außen sieht man das nur mit dem Auge – dieser
Mess-Build zählt es im Treiber mit.

```bash
# gepatchten Treiber erzeugen (einmalig) und damit bauen
mkdir -p build/diag
docker run --rm --entrypoint cat espressif/idf:v6.0 /opt/esp/idf/components/esp_lcd/rgb/esp_lcd_panel_rgb.c > build/diag/esp_lcd_panel_rgb.c
patch build/diag/esp_lcd_panel_rgb.c tools/diag/rgb_messung.patch
docker run --rm -v "$PWD":/project -v "$PWD/build/diag/esp_lcd_panel_rgb.c":/opt/esp/idf/components/esp_lcd/rgb/esp_lcd_panel_rgb.c \
  -w /project espressif/idf:v6.0 idf.py build
# flashen, dann Dauerlast (klickt sich durch die Bildschirme) und Auswertung:
python3 tools/diag/stress.py 6
```

Ausgabe des Debug-Befehls `lcd` (setzt die Zähler zurück):
- `STOER`: Stücke, die zu spät kamen (sichtbares Zucken) – muss 0 sein
- `versatz`: Bilder, die um ein Stück verrutscht waren – muss 0 sein
- `spaet+kopie`: knappster Fall in µs; die Grenze ist 2 × P (P = Dauer eines Stücks, 911 µs)
- `kopie<150/300/600/900/mehr`: wie lange das Kopieren eines Stücks dauerte (langsam = PSRAM gerade stark belastet)

Stand 14.11.2026, 90 s Dauerlast: vorher (Display-Unterbrechung auf Kern 0) 32 verrutschte Bilder und
`spaet+kopie` bis 1805 µs; jetzt (Kern 1, siehe `main.c`) 0 und höchstens ~1200 µs.

Ohne den Patch meldet `lcd` nur „keine Messung in diesem Build“.
