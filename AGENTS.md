# AGENTS.md

- Ziel: Panda Touch (ESP32-S3) als Touch-Bedienteil für einen Bambu P1S. Steuerung per MQTT (TLS, Port 8883, User `bblp`, Passwort = Access Code, Topics `device/<serial>/report|request`). Nur mit **Developer Mode** am Drucker; sonst kommt `err_code 84033543`.
- Gebaut wird über GitHub Actions (`.github/workflows/build.yml`, ESP-IDF v6.0), Artefakt `firmware.bin` (merged, ab Adresse 0 flashen).
- **Geheimnisse** (WLAN, Access Code, Seriennummer) nie ins Git: `main/secrets.h` ist ignoriert; später per Einrichtungsseite/NVS.
- Sprache Deutsch (Kommentare, Commits, UI-Texte).
- Original-Firmware-Backup (`pandatouch_original.bin`) nie committen.
- Steckbrief im Wiki: `topics/vibe-coding/wiki/topics/panda-touch-firmware.md`.
