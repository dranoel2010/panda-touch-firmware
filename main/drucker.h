/**
 * Anbindung an den Bambu P1S: WLAN, Uhrzeit (SNTP) und MQTT (TLS, Port 8883, Developer Mode).
 *
 * Läuft in eigenen Tasks. Die Oberfläche holt sich den aktuellen Stand mit drucker_holen()
 * (aus dem LVGL-Task heraus) und schickt Befehle mit drucker_befehl()/drucker_gcode().
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool eingerichtet;   /* Zugangsdaten vorhanden (main/secrets.h) */
    bool wlan;           /* WLAN verbunden, IP bekommen */
    bool verbunden;      /* MQTT verbunden */
    bool daten;          /* mindestens ein Bericht des Druckers empfangen */
    bool abgelehnt;      /* MQTT-Anmeldung abgelehnt (Access Code / Developer Mode) */
    char ssid[33];
    int rssi;

    char zustand[12];    /* gcode_state: IDLE, PREPARE, RUNNING, PAUSE, FINISH, FAILED … */
    int stufe;           /* stg_cur: 1 Nivellierung, 2/7 Aufheizen … */
    uint32_t fehler;     /* print_error */
    int prozent, rest_min, schicht, schichten;
    char datei[48];
    int duese, duese_soll, bett, bett_soll, bauraum;
    int tempo;           /* 1 Leise, 2 Standard, 3 Sport, 4 Ludicrous */
    int luefter[3];      /* Bauteil, Hilfs, Bauraum in % */
    bool licht;
    bool ams;
    char material[5][8]; /* 4 AMS-Slots + externe Spule */
    int slot_aktiv;      /* 0–3 AMS, 4 extern, -1 keiner */
    int feuchte;         /* AMS-Luftfeuchte-Stufe 1–5, 0 unbekannt */
} drucker_t;

void drucker_start(void);
/* Kopiert den Stand, wenn er sich seit *stand geändert hat; true = neu */
bool drucker_holen(drucker_t *aus, uint32_t *stand);
void drucker_neu_verbinden(void);

/* Befehle (werden still verworfen, wenn keine Verbindung besteht; Rückgabe false) */
bool drucker_druck_befehl(const char *befehl);             /* pause, resume, stop, unload_filament */
bool drucker_tempo(int stufe);
bool drucker_gcode(const char *gcode);
bool drucker_licht(bool an);
bool drucker_filament_laden(int slot, int temperatur);     /* slot 0–3 AMS, 4 extern */

const char *drucker_ip(void);
const char *drucker_seriennummer(void);
