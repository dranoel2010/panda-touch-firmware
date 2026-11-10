# Panda-Touch-Firmware

Eigene Firmware für den BIGTREETECH Panda Touch (ESP32-S3, 5″ 800×480, GT911-Touch),
die einen Bambu Lab P1S direkt per MQTT im LAN steuert (Developer Mode nötig).
Baut auf dem BSP [fmauneko/pandatouch](https://github.com/fmauneko/pandatouch-bsp) (MIT) und LVGL auf.

## Bauen
GitHub Actions baut bei jedem Push `firmware.bin` (Artefakt „firmware“). Lokal: `idf.py build` (ESP-IDF ≥ 5.3, getestet mit v6.0).

## Flashen (Mac)
```bash
python3 -m pip install --user esptool
python3 -m esptool --chip esp32s3 --port /dev/cu.usbmodemXXXX write_flash 0 firmware.bin
```
**Vorher einmal die Original-Firmware sichern:**
`python3 -m esptool --port /dev/cu.usbmodemXXXX read_flash 0 0x1000000 pandatouch_original.bin`
(Zurückspielen: `write_flash 0 pandatouch_original.bin`.)

## Stand
- v0.1.0: Display + Touch-Test
- v0.1.1: Helligkeitsregler (wird gespeichert)
