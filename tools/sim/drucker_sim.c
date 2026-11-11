/* Schein-Drucker für den Simulator (make sim_live): gleiche Schnittstelle wie main/drucker.c,
 * Zustand wird über Aktionen „dr:…“ gesetzt, z. B. dr:zustand=RUNNING dr:prozent=42 dr:dateien=3 */
#include "drucker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static drucker_t d = {
    .eingerichtet = true, .wlan = true, .verbunden = true, .daten = true, .ssid = "Heimnetz", .rssi = -55,
    .zustand = "IDLE", .duese = 24, .bett = 22, .tempo = 2, .ams = true,
    .material = { "PETG", "PLA", "PETG", "PETG", "TPU" }, .farbe = { 0xffffff, 0x3399cc, 0xcc6633, 0x555555, 0x8899aa },
    .slot_aktiv = 0, .feuchte = 7,
};
static uint32_t stand = 1;
static dateiliste_t liste;
static uint32_t liste_stand = 1;
static int dateien_n = 3;
char drucker_sim_letzter_befehl[256];

static const druckdatei_t BEISPIELE[3] = {
    { .name = "Version 2 – 0.16 mm Layers & Smoothing.3mf", .pfad = "/cache/Version 2 – 0.16 mm Layers & Smoothing.3mf",
      .platte = 1, .minuten = 141, .gramm = 92, .n_fil = 1, .fil_typ = { "PLA" }, .fil_farbe = { 0xF2754E } },
    { .name = "2 colors.3mf", .pfad = "/cache/2 colors.3mf", .platte = 1, .minuten = 24, .gramm = 8, .n_fil = 2,
      .fil_typ = { "PLA", "PETG" }, .fil_farbe = { 0xffffff, 0x161616 } },
    { .name = "lockcard holder.stl.3mf", .pfad = "/cache/lockcard holder.stl.3mf", .platte = 1, .minuten = -1, .gramm = -1 },
};

bool drucker_holen(drucker_t *aus, uint32_t *s) { if (*s == stand) return false; *aus = d; *s = stand; return true; }
bool dateien_holen(dateiliste_t *aus, uint32_t *s) { if (*s == liste_stand) return false; *aus = liste; *s = liste_stand; return true; }
void dateien_laden(void)
{
    liste.status = 2;
    liste.anzahl = dateien_n;
    for (int i = 0; i < dateien_n; i++) liste.d[i] = BEISPIELE[i];
    liste_stand++;
}
void drucker_start(void) {}
void drucker_neu_verbinden(void) {}
static bool merke(const char *b) { snprintf(drucker_sim_letzter_befehl, sizeof(drucker_sim_letzter_befehl), "%s", b); printf("Befehl: %s\n", b); return d.verbunden; }
bool drucker_druck_befehl(const char *b) { return merke(b); }
bool drucker_tempo(int s) { char b[32]; snprintf(b, sizeof(b), "tempo %d", s); d.tempo = s; return merke(b); }
bool drucker_gcode(const char *g) { return merke(g); }
bool drucker_licht(bool an) { d.licht = an; stand++; return merke(an ? "licht an" : "licht aus"); }
bool drucker_filament_laden(int slot, int t) { char b[32]; snprintf(b, sizeof(b), "laden %d %d", slot, t); return merke(b); }
bool drucker_filament_entladen(int t) { char b[32]; snprintf(b, sizeof(b), "entladen %d", t); return merke(b); }
bool drucker_material(int slot, const char *typ) { snprintf(d.material[slot], 8, "%s", typ); stand++; return merke(typ); }
bool drucker_drucken(const druckdatei_t *f, bool n, bool fl, bool a, bool z)
{
    char b[200];
    snprintf(b, sizeof(b), "drucken %s platte %d niv %d flow %d ams %d zeitraffer %d", f->pfad, f->platte, n, fl, a, z);
    return merke(b);
}
const char *drucker_ip(void) { return "192.168.1.50"; }
const char *drucker_seriennummer(void) { return "01P00A000000000"; }

/* dr:feld=wert */
void drucker_sim_setzen(const char *a)
{
    char k[24], v[64];
    if (sscanf(a, "dr:%23[^=]=%63[^\n]", k, v) < 1) return;
    if (!strcmp(k, "zustand")) snprintf(d.zustand, sizeof(d.zustand), "%s", v);
    else if (!strcmp(k, "stufe")) d.stufe = atoi(v);
    else if (!strcmp(k, "prozent")) d.prozent = atoi(v);
    else if (!strcmp(k, "rest")) d.rest_min = atoi(v);
    else if (!strcmp(k, "schicht")) { d.schicht = atoi(v); d.schichten = atoi(strchr(v, '/') ? strchr(v, '/') + 1 : "0"); }
    else if (!strcmp(k, "datei")) snprintf(d.datei, sizeof(d.datei), "%s", v);
    else if (!strcmp(k, "duese")) { d.duese = atoi(v); d.duese_soll = atoi(strchr(v, '/') ? strchr(v, '/') + 1 : v); }
    else if (!strcmp(k, "bett")) { d.bett = atoi(v); d.bett_soll = atoi(strchr(v, '/') ? strchr(v, '/') + 1 : v); }
    else if (!strcmp(k, "fehler")) d.fehler = strtoul(v, NULL, 0);
    else if (!strcmp(k, "hms")) { d.hms_n = 1; d.hms_attr = 0x03000100; d.hms_code = 0x00010007; if (!atoi(v)) d.hms_n = d.hms_attr = d.hms_code = 0; }
    else if (!strcmp(k, "offline")) d.verbunden = !atoi(v);
    else if (!strcmp(k, "nie")) d.daten = !atoi(v);
    else if (!strcmp(k, "abgelehnt")) d.abgelehnt = atoi(v);
    else if (!strcmp(k, "ams")) d.ams = atoi(v);
    else if (!strcmp(k, "licht")) d.licht = atoi(v);
    else if (!strcmp(k, "dateien")) { dateien_n = atoi(v); if (liste.status == 2) dateien_laden(); }
    else if (!strcmp(k, "dateifehler")) { liste.status = 3; liste.anzahl = 0; liste_stand++; }
    stand++;
}
