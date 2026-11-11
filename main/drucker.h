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
    uint32_t farbe[5];   /* RGB der Spulen (für die Zuordnung beim Druckstart) */
    int slot_aktiv;      /* 0–3 AMS, 4 extern, -1 keiner */
    int feuchte;         /* AMS-Luftfeuchte in %, -1 unbekannt */
    int hms_n;           /* Anzahl HMS-Meldungen */
    uint32_t hms_attr, hms_code;   /* erste HMS-Meldung */
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
bool drucker_filament_entladen(int temperatur);
bool drucker_material(int slot, const char *typ);          /* Material eines Slots am Drucker einstellen */

/* ------------------------------------------------------------------ Druckdateien (FTPS, SD-Karte des Druckers) */

#define DATEIEN_MAX 3
#define FILAMENTE_MAX 8
typedef struct {
    char name[48];        /* Dateiname ohne Pfad */
    char pfad[112];       /* z. B. /cache/teil.3mf */
    uint32_t groesse;
    int64_t zeit;         /* Änderungszeit (Unix) */
    int platte;           /* Metadata/plate_N.gcode */
    int minuten;          /* geschätzte Druckzeit, -1 unbekannt */
    int gramm;            /* Filamentgewicht, -1 unbekannt */
    int n_fil;
    char fil_typ[FILAMENTE_MAX][8];
    uint32_t fil_farbe[FILAMENTE_MAX];
} druckdatei_t;

typedef struct {
    int status;           /* 0 nie geladen, 1 lädt, 2 fertig, 3 Fehler */
    int anzahl;
    druckdatei_t d[DATEIEN_MAX];
} dateiliste_t;

void dateien_laden(void);                                   /* im Hintergrund neu einlesen */
bool dateien_holen(dateiliste_t *aus, uint32_t *stand);     /* true = neu */
bool drucker_drucken(const druckdatei_t *f, bool nivellieren, bool flow_kali, bool ams, bool zeitraffer);

const char *drucker_ip(void);
const char *drucker_seriennummer(void);
