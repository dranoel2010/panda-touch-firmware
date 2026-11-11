/**
 * Anbindung an den Bambu P1S (siehe drucker.h).
 *
 * Zugangsdaten kommen aus main/secrets.h (nie im Git, Vorlage: main/secrets.h.example).
 * Der Drucker schickt nach "pushall" einmal den vollständigen Stand und danach nur noch
 * die geänderten Felder – deshalb wird jeder Bericht in den bestehenden Stand eingemischt.
 */
#include "drucker.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/timers.h"
#include "mqtt_client.h"

#if __has_include("secrets.h")
#include "secrets.h"
#endif
#ifndef WLAN_SSID
#define WLAN_SSID ""
#endif
#ifndef WLAN_PASSWORT
#define WLAN_PASSWORT ""
#endif
#ifndef DRUCKER_IP
#define DRUCKER_IP ""
#endif
#ifndef DRUCKER_ACCESS_CODE
#define DRUCKER_ACCESS_CODE ""
#endif
#ifndef DRUCKER_SERIENNUMMER
#define DRUCKER_SERIENNUMMER ""
#endif

static const char *TAG = "drucker";

static SemaphoreHandle_t sperre;
static drucker_t d;
static uint32_t d_stand = 1;
static esp_mqtt_client_handle_t mqtt;
static bool mqtt_gestartet;
static char topic_bericht[64], topic_befehl[64];
static uint32_t sequenz;
static TimerHandle_t wlan_timer;

/* Nachrichten kommen in Stücken (Puffer kleiner als ein voller Bericht) -> hier zusammensetzen */
#define NACHRICHT_MAX (96 * 1024)
static char *nachricht;

const char *drucker_ip(void) { return DRUCKER_IP; }
const char *drucker_seriennummer(void) { return DRUCKER_SERIENNUMMER; }

static void *psram_malloc(size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }

#define MIT_SPERRE(code) do { xSemaphoreTake(sperre, portMAX_DELAY); code; d_stand++; xSemaphoreGive(sperre); } while (0)

bool drucker_holen(drucker_t *aus, uint32_t *stand)
{
    if (!sperre) return false;
    xSemaphoreTake(sperre, portMAX_DELAY);
    bool neu = *stand != d_stand;
    if (neu) { *aus = d; *stand = d_stand; }
    xSemaphoreGive(sperre);
    return neu;
}

/* ------------------------------------------------------------------ Bericht auswerten */

/* Zahl, die je nach Feld als Zahl oder als Text kommt */
static bool zahl(const cJSON *o, const char *k, double *v)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(i)) { *v = i->valuedouble; return true; }
    if (cJSON_IsString(i) && i->valuestring[0]) { *v = strtod(i->valuestring, NULL); return true; }
    return false;
}

static void text(const cJSON *o, const char *k, char *ziel, size_t n)
{
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(i)) snprintf(ziel, n, "%s", i->valuestring);
}

static int luefter_prozent(double stufe) { return (int)(stufe * 100.0 / 15.0 / 10.0 + 0.5) * 10; }

static void material_setzen(int i, const cJSON *tray)
{
    char typ[8] = "", farbe[12] = "";
    text(tray, "tray_type", typ, sizeof(typ));
    snprintf(d.material[i], 8, "%s", typ[0] ? typ : "–");
    text(tray, "tray_color", farbe, sizeof(farbe));
    if (farbe[0]) d.farbe[i] = (uint32_t)(strtoul(farbe, NULL, 16) >> 8);   /* RRGGBBAA */
}

static void bericht(const cJSON *p)
{
    double v;
    text(p, "gcode_state", d.zustand, sizeof(d.zustand));
    if (zahl(p, "stg_cur", &v)) d.stufe = (int)v;
    if (zahl(p, "print_error", &v)) d.fehler = (uint32_t)v;
    if (zahl(p, "mc_percent", &v)) d.prozent = (int)v;
    if (zahl(p, "mc_remaining_time", &v)) d.rest_min = (int)v;
    if (zahl(p, "layer_num", &v)) d.schicht = (int)v;
    if (zahl(p, "total_layer_num", &v)) d.schichten = (int)v;
    char name[48] = "";
    text(p, "subtask_name", name, sizeof(name));
    if (name[0]) {
        size_t l = strlen(name);
        if (l < 4 || strcmp(name + l - 4, ".3mf")) strncat(name, ".3mf", sizeof(name) - l - 1);
        snprintf(d.datei, sizeof(d.datei), "%s", name);
    }
    if (zahl(p, "nozzle_temper", &v)) d.duese = (int)(v + 0.5);
    if (zahl(p, "nozzle_target_temper", &v)) d.duese_soll = (int)(v + 0.5);
    if (zahl(p, "bed_temper", &v)) d.bett = (int)(v + 0.5);
    if (zahl(p, "bed_target_temper", &v)) d.bett_soll = (int)(v + 0.5);
    if (zahl(p, "chamber_temper", &v)) d.bauraum = (int)(v + 0.5);
    if (zahl(p, "spd_lvl", &v)) d.tempo = (int)v;
    if (zahl(p, "cooling_fan_speed", &v)) d.luefter[0] = luefter_prozent(v);
    if (zahl(p, "big_fan1_speed", &v)) d.luefter[1] = luefter_prozent(v);
    if (zahl(p, "big_fan2_speed", &v)) d.luefter[2] = luefter_prozent(v);

    const cJSON *lichter = cJSON_GetObjectItemCaseSensitive(p, "lights_report"), *l;
    cJSON_ArrayForEach(l, lichter) {
        char node[24] = "", mode[8] = "";
        text(l, "node", node, sizeof(node));
        text(l, "mode", mode, sizeof(mode));
        if (!strcmp(node, "chamber_light")) d.licht = strcmp(mode, "off") != 0;
    }

    const cJSON *ams = cJSON_GetObjectItemCaseSensitive(p, "ams");
    if (cJSON_IsObject(ams)) {
        const cJSON *einheiten = cJSON_GetObjectItemCaseSensitive(ams, "ams");
        if (cJSON_IsArray(einheiten)) {
            const cJSON *erste = cJSON_GetArrayItem(einheiten, 0);
            if (erste) d.ams = true;   /* Teilberichte können eine leere Liste schicken -> nicht als „kein AMS“ werten */
            if (erste) {
                if (zahl(erste, "humidity_raw", &v)) d.feuchte = (int)(v + 0.5);   /* ältere Drucker-Firmware: nur Stufe */
                const cJSON *t;
                cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(erste, "tray")) {
                    if (!zahl(t, "id", &v) || v < 0 || v > 3) continue;
                    material_setzen((int)v, t);
                }
            }
        }
        if (zahl(ams, "ams_exist_bits", &v)) d.ams = v != 0;
        if (zahl(ams, "tray_now", &v)) {
            int n = (int)v;
            d.slot_aktiv = n >= 0 && n <= 3 ? n : n == 254 ? 4 : -1;
        }
    }
    const cJSON *vt = cJSON_GetObjectItemCaseSensitive(p, "vt_tray");
    if (cJSON_IsObject(vt)) material_setzen(4, vt);
}

static void nachricht_auswerten(const char *daten, int laenge)
{
    cJSON *wurzel = cJSON_ParseWithLength(daten, laenge);
    if (!wurzel) { ESP_LOGW(TAG, "Bericht nicht lesbar (%d Byte)", laenge); return; }
    const cJSON *p = cJSON_GetObjectItemCaseSensitive(wurzel, "print");
    if (cJSON_IsObject(p)) MIT_SPERRE(bericht(p); d.daten = true);
    cJSON_Delete(wurzel);
}

/* ------------------------------------------------------------------ Befehle */

static bool senden(cJSON *wurzel)
{
    bool ok = false;
    if (mqtt && d.verbunden) {
        char *s = cJSON_PrintUnformatted(wurzel);
        if (s) {
            /* QoS 0 und nur in die Warteschlange: QoS 1 wiederholt unbestätigte Befehle (der Drucker
             * referenzierte so endlos), und publish() blockiert die Oberfläche, solange der MQTT-Task
             * gerade liest/schreibt */
            ok = esp_mqtt_client_enqueue(mqtt, topic_befehl, s, 0, 0, 0, true) >= 0;
            ESP_LOGI(TAG, "-> %s", s);
            cJSON_free(s);
        }
    }
    cJSON_Delete(wurzel);
    return ok;
}

static cJSON *befehl_neu(const char *gruppe, const char *befehl, cJSON **inhalt)
{
    cJSON *w = cJSON_CreateObject();
    *inhalt = cJSON_AddObjectToObject(w, gruppe);
    char seq[12];
    snprintf(seq, sizeof(seq), "%lu", (unsigned long)++sequenz);
    cJSON_AddStringToObject(*inhalt, "sequence_id", seq);
    cJSON_AddStringToObject(*inhalt, "command", befehl);
    return w;
}

bool drucker_druck_befehl(const char *befehl)
{
    cJSON *p, *w = befehl_neu("print", befehl, &p);
    cJSON_AddStringToObject(p, "param", "");
    return senden(w);
}

bool drucker_tempo(int stufe)
{
    char s[4];
    snprintf(s, sizeof(s), "%d", stufe);
    cJSON *p, *w = befehl_neu("print", "print_speed", &p);
    cJSON_AddStringToObject(p, "param", s);
    bool ok = senden(w);
    if (ok) MIT_SPERRE(d.tempo = stufe);
    return ok;
}

bool drucker_gcode(const char *gcode)
{
    cJSON *p, *w = befehl_neu("print", "gcode_line", &p);
    cJSON_AddStringToObject(p, "param", gcode);
    return senden(w);
}

bool drucker_licht(bool an)
{
    cJSON *p, *w = befehl_neu("system", "ledctrl", &p);
    cJSON_AddStringToObject(p, "led_node", "chamber_light");
    cJSON_AddStringToObject(p, "led_mode", an ? "on" : "off");
    cJSON_AddNumberToObject(p, "led_on_time", 500);
    cJSON_AddNumberToObject(p, "led_off_time", 500);
    cJSON_AddNumberToObject(p, "loop_times", 0);
    cJSON_AddNumberToObject(p, "interval_time", 0);
    bool ok = senden(w);
    if (ok) MIT_SPERRE(d.licht = an);
    return ok;
}

bool drucker_filament_laden(int slot, int temperatur)
{
    cJSON *p, *w = befehl_neu("print", "ams_change_filament", &p);
    cJSON_AddNumberToObject(p, "target", slot == 4 ? 254 : slot);
    cJSON_AddNumberToObject(p, "curr_temp", temperatur);
    cJSON_AddNumberToObject(p, "tar_temp", temperatur);
    return senden(w);
}

static void alles_anfordern(void)
{
    cJSON *p, *w = befehl_neu("pushing", "pushall", &p);
    cJSON_AddNumberToObject(p, "version", 1);
    cJSON_AddNumberToObject(p, "push_target", 1);
    senden(w);
}

/* ------------------------------------------------------------------ MQTT */

static void mqtt_ereignis(void *arg, esp_event_base_t base, int32_t id, void *daten)
{
    esp_mqtt_event_handle_t e = daten;
    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG, "MQTT verbunden");
        MIT_SPERRE(d.verbunden = true; d.abgelehnt = false);
        esp_mqtt_client_subscribe(mqtt, topic_bericht, 0);
        alles_anfordern();
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "MQTT getrennt");
        MIT_SPERRE(d.verbunden = false);
        break;
    case MQTT_EVENT_ERROR:
        if (e->error_handle && e->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            ESP_LOGW(TAG, "MQTT-Anmeldung abgelehnt (%d)", e->error_handle->connect_return_code);
            MIT_SPERRE(d.abgelehnt = true);
        }
        break;
    case MQTT_EVENT_DATA:
        if (e->total_data_len > NACHRICHT_MAX) break;
        if (e->total_data_len == e->data_len) { nachricht_auswerten(e->data, e->data_len); break; }
        memcpy(nachricht + e->current_data_offset, e->data, e->data_len);
        if (e->current_data_offset + e->data_len >= e->total_data_len) nachricht_auswerten(nachricht, e->total_data_len);
        break;
    default: break;
    }
}

static void mqtt_starten(void)
{
    if (mqtt_gestartet) return;
    char uri[48];
    snprintf(uri, sizeof(uri), "mqtts://%s:8883", DRUCKER_IP);
    esp_mqtt_client_config_t c = {
        .broker.address.uri = uri,
        /* Der Drucker hat ein selbst signiertes Zertifikat (lokales Netz) */
        .broker.verification.skip_cert_common_name_check = true,
        .credentials.username = "bblp",
        .credentials.client_id = "panda-touch",
        .credentials.authentication.password = DRUCKER_ACCESS_CODE,
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 5000,
        .network.timeout_ms = 10000,
        .buffer.size = 4096,
        .buffer.out_size = 1024,
        .task.stack_size = 8192,
    };
    mqtt = esp_mqtt_client_init(&c);
    if (!mqtt) { ESP_LOGE(TAG, "MQTT nicht startbar"); return; }
    esp_mqtt_client_register_event(mqtt, ESP_EVENT_ANY_ID, mqtt_ereignis, NULL);
    esp_mqtt_client_start(mqtt);
    mqtt_gestartet = true;
}

void drucker_neu_verbinden(void)
{
    if (mqtt_gestartet && d.wlan) esp_mqtt_client_reconnect(mqtt);
    else if (d.eingerichtet) esp_wifi_connect();
}

/* ------------------------------------------------------------------ WLAN */

static void wlan_nochmal(TimerHandle_t t) { (void)t; esp_wifi_connect(); }

static void signal_messen(TimerHandle_t t)
{
    (void)t;
    wifi_ap_record_t ap;
    if (d.wlan && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) MIT_SPERRE(d.rssi = ap.rssi);
}

static void wlan_ereignis(void *arg, esp_event_base_t base, int32_t id, void *daten)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *g = daten;
        ESP_LOGW(TAG, "WLAN getrennt, Grund %d, RSSI %d", g->reason, g->rssi);
        MIT_SPERRE(d.wlan = false);
        xTimerStart(wlan_timer, 0);   /* in 5 s erneut */
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ESP_LOGI(TAG, "WLAN verbunden");
        wifi_ap_record_t ap;
        int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
        MIT_SPERRE(d.wlan = true; d.rssi = rssi);
        mqtt_starten();
    }
}

void drucker_start(void)
{
    sperre = xSemaphoreCreateMutex();
    nachricht = heap_caps_malloc(NACHRICHT_MAX, MALLOC_CAP_SPIRAM);
    /* Viele kleine JSON-Knoten: in den PSRAM, damit der interne RAM nicht zerstückelt */
    cJSON_Hooks hooks = { .malloc_fn = psram_malloc, .free_fn = heap_caps_free };
    cJSON_InitHooks(&hooks);

    memset(&d, 0, sizeof(d));
    d.slot_aktiv = -1;
    d.feuchte = -1;
    for (int i = 0; i < 5; i++) strcpy(d.material[i], "–");
    snprintf(d.ssid, sizeof(d.ssid), "%s", WLAN_SSID);
    d.eingerichtet = WLAN_SSID[0] && DRUCKER_IP[0] && DRUCKER_ACCESS_CODE[0] && DRUCKER_SERIENNUMMER[0];
    if (!d.eingerichtet) {
        ESP_LOGW(TAG, "Keine Zugangsdaten (main/secrets.h fehlt oder ist unvollständig)");
        return;
    }
    snprintf(topic_bericht, sizeof(topic_bericht), "device/%s/report", DRUCKER_SERIENNUMMER);
    snprintf(topic_befehl, sizeof(topic_befehl), "device/%s/request", DRUCKER_SERIENNUMMER);

    /* Uhrzeit fürs voraussichtliche Druckende */
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    /* Fehler hier dürfen das Gerät nie abstürzen lassen: dann eben ohne Drucker (Anzeige „offline“) */
    if (esp_netif_init() != ESP_OK) return;
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return;
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WLAN nicht startbar (%s), intern frei %u", esp_err_to_name(err),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        MIT_SPERRE(d.eingerichtet = false);
        return;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wlan_ereignis, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wlan_ereignis, NULL);
    wifi_config_t wc = { 0 };
    snprintf((char *)wc.sta.ssid, sizeof(wc.sta.ssid), "%s", WLAN_SSID);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", WLAN_PASSWORT);
    wc.sta.threshold.authmode = WLAN_PASSWORT[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    wlan_timer = xTimerCreate("wlan", pdMS_TO_TICKS(5000), pdFALSE, NULL, wlan_nochmal);
    xTimerStart(xTimerCreate("signal", pdMS_TO_TICKS(10000), pdTRUE, NULL, signal_messen), 0);
    if (esp_wifi_start() != ESP_OK) { ESP_LOGE(TAG, "WLAN-Start fehlgeschlagen"); return; }
    /* weniger Sendeleistung (13 dBm statt 20): Funk + volles Hintergrundlicht ließen die
     * Versorgung über USB einbrechen; im Haus reicht das für den Router locker */
    esp_wifi_set_max_tx_power(52);

    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
}

/* ------------------------------------------------------------------ Druckdateien über FTPS (Port 990, implizites TLS)
 *
 * Liste aus / und /cache, neueste zuerst. Für jede Datei wird nur das Inhaltsverzeichnis des
 * 3MF-Archivs (Ende der Datei) und die kleine Metadata/slice_info.config geladen: daraus kommen
 * Platte, Druckzeit, Gewicht und Filamente. 3MF ohne geslicte Platte sind nicht druckbar und fehlen.
 */
#include "esp_tls.h"
#include "miniz.h"

static dateiliste_t liste;
static uint32_t liste_stand = 1;
static volatile bool liste_laeuft;

bool dateien_holen(dateiliste_t *aus, uint32_t *stand)
{
    if (!sperre) return false;
    xSemaphoreTake(sperre, portMAX_DELAY);
    bool neu = *stand != liste_stand;
    if (neu) { *aus = liste; *stand = liste_stand; }
    xSemaphoreGive(sperre);
    return neu;
}

typedef struct {
    esp_tls_t *tls;
    char puf[512];
    int pos, len;
} ftp_t;

static esp_tls_t *tls_verbinden(int port)
{
    esp_tls_cfg_t c = { .timeout_ms = 8000, .skip_common_name = true };
    esp_tls_t *t = esp_tls_init();
    if (!t) return NULL;
    if (esp_tls_conn_new_sync(DRUCKER_IP, strlen(DRUCKER_IP), port, &c, t) != 1) {
        esp_tls_conn_destroy(t);
        return NULL;
    }
    return t;
}

static int ftp_zeichen(ftp_t *f)
{
    if (f->pos >= f->len) {
        int n = esp_tls_conn_read(f->tls, f->puf, sizeof(f->puf));
        if (n <= 0) return -1;
        f->pos = 0;
        f->len = n;
    }
    return (unsigned char)f->puf[f->pos++];
}

/* eine Zeile (ohne CRLF); false bei Verbindungsende */
static bool ftp_zeile(ftp_t *f, char *z, size_t n)
{
    size_t l = 0;
    for (;;) {
        int c = ftp_zeichen(f);
        if (c < 0) return false;
        if (c == '\n') break;
        if (c != '\r' && l + 1 < n) z[l++] = (char)c;
    }
    z[l] = 0;
    return true;
}

/* Antwortcode (mehrzeilige Antworten „123-…“ bis „123 …“); text optional */
static int ftp_antwort(ftp_t *f, char *text, size_t n)
{
    char z[160];
    if (!ftp_zeile(f, z, sizeof(z))) return -1;
    int code = atoi(z);
    if (z[3] == '-') {
        char ende[5];
        snprintf(ende, sizeof(ende), "%.3s ", z);
        do { if (!ftp_zeile(f, z, sizeof(z))) return -1; } while (strncmp(z, ende, 4));
    }
    if (text) strlcpy(text, z, n);
    return code;
}

static int ftp_befehl(ftp_t *f, const char *befehl, char *text, size_t n)
{
    char b[160];
    int l = snprintf(b, sizeof(b), "%s\r\n", befehl);
    if (esp_tls_conn_write(f->tls, b, l) != l) return -1;
    return ftp_antwort(f, text, n);
}

static bool ftp_anmelden(ftp_t *f)
{
    memset(f, 0, sizeof(*f));
    f->tls = tls_verbinden(990);
    if (!f->tls) return false;
    char b[64];
    snprintf(b, sizeof(b), "PASS %s", DRUCKER_ACCESS_CODE);
    return ftp_antwort(f, NULL, 0) == 220 && ftp_befehl(f, "USER bblp", NULL, 0) == 331 && ftp_befehl(f, b, NULL, 0) == 230 &&
           ftp_befehl(f, "TYPE I", NULL, 0) == 200 && ftp_befehl(f, "PBSZ 0", NULL, 0) == 200 &&
           ftp_befehl(f, "PROT P", NULL, 0) == 200;
}

static void ftp_ende(ftp_t *f)
{
    if (f->tls) esp_tls_conn_destroy(f->tls);
    f->tls = NULL;
}

/* Datenverbindung: PASV, Befehl schicken, dann verbinden (der Drucker startet TLS erst nach dem Befehl) */
static esp_tls_t *ftp_daten(ftp_t *f, const char *befehl)
{
    char a[96];
    if (ftp_befehl(f, "PASV", a, sizeof(a)) != 227) return NULL;
    const char *k = strchr(a, '(');
    int h1, h2, h3, h4, p1, p2;
    if (!k || sscanf(k, "(%d,%d,%d,%d,%d,%d)", &h1, &h2, &h3, &h4, &p1, &p2) != 6) return NULL;
    char b[160];
    int l = snprintf(b, sizeof(b), "%s\r\n", befehl);
    if (esp_tls_conn_write(f->tls, b, l) != l) return NULL;
    esp_tls_t *dt = tls_verbinden(p1 * 256 + p2);
    if (!dt) { ftp_antwort(f, NULL, 0); return NULL; }
    int c = ftp_antwort(f, NULL, 0);   /* 150 */
    if (c != 150 && c != 125) { esp_tls_conn_destroy(dt); return NULL; }
    return dt;
}

/* Datenverbindung beenden und die Abschlussantwort (226, bei Abbruch 426) abholen */
static void ftp_daten_ende(ftp_t *f, esp_tls_t *dt)
{
    esp_tls_conn_destroy(dt);
    ftp_antwort(f, NULL, 0);
}

/* nach einem abgebrochenen Transfer wieder in Tritt kommen: NOOP schicken und alles bis zu dessen 200 lesen */
static void ftp_sync(ftp_t *f)
{
    if (esp_tls_conn_write(f->tls, "NOOP\r\n", 6) != 6) return;
    for (int i = 0; i < 5; i++) if (ftp_antwort(f, NULL, 0) == 200) return;
}

static const char *const MONATE[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

/* Zeile aus LIST: "-rw-rw-rw- 1 root root 474963 Nov 05 23:36 platte_A_c12.3mf" */
static bool list_zeile(const char *z, const char *ordner, druckdatei_t *aus)
{
    if (z[0] != '-') return false;
    char mon[4], uhr[8];
    unsigned long groesse;
    int tag, pos = 0;
    if (sscanf(z, "%*s %*s %*s %*s %lu %3s %d %7s %n", &groesse, mon, &tag, uhr, &pos) != 4 || !pos) return false;
    const char *name = z + pos;
    size_t l = strlen(name);
    if (l < 5 || strcasecmp(name + l - 4, ".3mf") || l >= sizeof(aus->name)) return false;
    memset(aus, 0, sizeof(*aus));
    snprintf(aus->name, sizeof(aus->name), "%s", name);
    snprintf(aus->pfad, sizeof(aus->pfad), "%s%s%s", ordner, strcmp(ordner, "/") ? "/" : "", name);
    aus->groesse = groesse;
    struct tm tm = { 0 };
    for (int i = 0; i < 12; i++) if (!strcmp(mon, MONATE[i])) tm.tm_mon = i;
    tm.tm_mday = tag;
    time_t jetzt = time(NULL);
    struct tm heute;
    localtime_r(&jetzt, &heute);
    if (strchr(uhr, ':')) {   /* aktuelles Jahr, außer der Monat liegt in der Zukunft */
        sscanf(uhr, "%d:%d", &tm.tm_hour, &tm.tm_min);
        tm.tm_year = heute.tm_year - (tm.tm_mon > heute.tm_mon ? 1 : 0);
    } else tm.tm_year = atoi(uhr) - 1900;
    tm.tm_isdst = -1;
    aus->zeit = mktime(&tm);
    aus->minuten = aus->gramm = -1;
    return true;
}

#define KANDIDATEN 10
static druckdatei_t *kand;
static int n_kand;

static void kandidat(const druckdatei_t *k)
{
    int i;
    if (n_kand < KANDIDATEN) i = n_kand++;
    else if (k->zeit > kand[KANDIDATEN - 1].zeit) i = KANDIDATEN - 1;
    else return;
    kand[i] = *k;
    for (; i > 0 && kand[i].zeit > kand[i - 1].zeit; i--) {   /* nach Zeit absteigend einsortieren */
        druckdatei_t t = kand[i];
        kand[i] = kand[i - 1];
        kand[i - 1] = t;
    }
}

static bool ordner_lesen(ftp_t *f, const char *ordner)
{
    char b[128];
    snprintf(b, sizeof(b), "LIST %s", ordner);
    esp_tls_t *dt = ftp_daten(f, b);
    if (!dt) return false;
    ftp_t d = { .tls = dt };
    char z[256];
    druckdatei_t k;
    int n = 0;
    while (ftp_zeile(&d, z, sizeof(z))) {
        n++;
        if (list_zeile(z, ordner, &k)) kandidat(&k);
    }
    ESP_LOGI(TAG, "%s: %d Einträge, %d Kandidaten", ordner, n, n_kand);
    ftp_daten_ende(f, dt);
    return true;
}

static uint16_t le16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static const char *xml_wert(const char *x, const char *schluessel, char *wert, size_t n)
{
    char such[48];
    snprintf(such, sizeof(such), "key=\"%s\" value=\"", schluessel);
    const char *p = strstr(x, such);
    if (!p) return NULL;
    p += strlen(such);
    size_t l = strcspn(p, "\"");
    if (l >= n) l = n - 1;
    memcpy(wert, p, l);
    wert[l] = 0;
    return wert;
}

static const char *attr(const char *tag, const char *name, char *wert, size_t n)
{
    char such[24];
    snprintf(such, sizeof(such), " %s=\"", name);
    const char *p = strstr(tag, such), *e = strchr(tag, '>');
    if (!p || (e && p > e)) return NULL;
    p += strlen(such);
    size_t l = strcspn(p, "\"");
    if (l >= n) l = n - 1;
    memcpy(wert, p, l);
    wert[l] = 0;
    return wert;
}

/* 3MF (ZIP) im Durchlauf lesen. Der FTP-Server des Druckers kann kein REST (nicht mittendrin anfangen),
 * also wird von vorne gelesen:
 *  - lokale Köpfe der Reihe nach: Platte (Metadata/plate_N.gcode) und slice_info.config direkt mitnehmen,
 *    abbrechen, sobald beides da ist (bei den meisten Dateien nach wenigen KB)
 *  - ohne Größen im Kopf (Datenbeschreiber, Bit 3) geht die Spur verloren; dann helfen die letzten 64 KB
 *    (Inhaltsverzeichnis am Dateiende) */
#define RING_N 65536
#define SI_MAX 16384

typedef struct {
    uint8_t *ring;
    uint32_t gesamt;          /* bisher gelesene Bytes */
    int zustand;              /* 0 Kopf lesen, 1 Daten überspringen, 2 slice_info sammeln, 3 Spur verloren */
    uint8_t kopf[30 + 128];
    int kopf_n, kopf_soll;
    uint32_t rest, daten_rest;
    int nach_extra;           /* nach dem Extra-Feld: 0 nächster Kopf, 1 Daten überspringen, 2 sammeln */
    uint8_t *si;
    uint32_t si_c, si_u, si_n;
    int si_meth;
    bool si_fertig;
    int platte;
} zipstrom_t;

static bool name_ist_platte(const char *name, int nl, int *platte)
{
    char n[40], rest[8];
    if (nl >= (int)sizeof(n)) return false;
    memcpy(n, name, nl);
    n[nl] = 0;
    int p;
    if (sscanf(n, "Metadata/plate_%d.gcod%1s", &p, rest) == 2 && !strcmp(n + nl - 6, ".gcode")) { *platte = p; return true; }
    return false;
}

/* Kopf (30 Byte + Name) vollständig: festlegen, was mit den Daten passiert */
static void strom_kopf_fertig(zipstrom_t *z)
{
    const uint8_t *k = z->kopf;
    int flag = le16(k + 6), nl = le16(k + 26);
    uint32_t c = le32(k + 18);
    const char *name = (const char *)k + 30;
    int p;
    if (name_ist_platte(name, nl, &p) && !z->platte) z->platte = p;
    z->nach_extra = 0;
    if ((flag & 8) && c == 0) { z->zustand = 3; return; }   /* Größe steht erst hinter den Daten */
    if (nl == 26 && !memcmp(name, "Metadata/slice_info.config", 26) && c <= SI_MAX && !z->si_fertig) {
        z->si_meth = le16(k + 8);
        z->si_c = c;
        z->si_u = le32(k + 22);
        z->si_n = 0;
        if (c) z->nach_extra = 2;
        else z->si_fertig = true;
    } else if (c) {
        z->daten_rest = c;
        z->nach_extra = 1;
    }
    z->rest = le16(k + 28);   /* erst das Extra-Feld überspringen */
    z->zustand = 4;
}

static void strom_futter(zipstrom_t *z, const uint8_t *p, int n)
{
    for (int i = 0; i < n; i++) z->ring[(z->gesamt + i) % RING_N] = p[i];
    z->gesamt += n;
    while (n > 0 && z->zustand != 3) {
        if (z->zustand == 0) {   /* Kopf sammeln */
            int soll = z->kopf_soll ? z->kopf_soll : 30;
            int m = soll - z->kopf_n < n ? soll - z->kopf_n : n;
            memcpy(z->kopf + z->kopf_n, p, m);
            z->kopf_n += m; p += m; n -= m;
            if (z->kopf_n < soll) break;
            if (!z->kopf_soll) {
                if (le32(z->kopf) != 0x04034b50) { z->zustand = 3; break; }   /* Inhaltsverzeichnis erreicht */
                int nl = le16(z->kopf + 26);
                if (nl > 128) { z->zustand = 3; break; }
                z->kopf_soll = 30 + nl;
                continue;
            }
            strom_kopf_fertig(z);
            z->kopf_n = z->kopf_soll = 0;
        } else if (z->zustand == 4 || z->zustand == 1) {   /* Extra-Feld bzw. Daten überspringen */
            uint32_t m = z->rest < (uint32_t)n ? z->rest : (uint32_t)n;
            p += m; n -= m; z->rest -= m;
            if (z->rest) break;
            if (z->zustand == 4 && z->nach_extra == 1) { z->zustand = 1; z->rest = z->daten_rest; }
            else if (z->zustand == 4 && z->nach_extra == 2) z->zustand = 2;
            else z->zustand = 0;
        } else if (z->zustand == 2) {   /* slice_info.config sammeln */
            uint32_t m = z->si_c - z->si_n < (uint32_t)n ? z->si_c - z->si_n : (uint32_t)n;
            memcpy(z->si + z->si_n, p, m);
            p += m; n -= m; z->si_n += m;
            if (z->si_n == z->si_c) { z->si_fertig = true; z->zustand = 0; }
        }
    }
}

/* Inhaltsverzeichnis aus den letzten 64 KB (wenn die Spur verloren ging) */
static void strom_ende(zipstrom_t *z)
{
    uint32_t n = z->gesamt < RING_N ? z->gesamt : RING_N, ab = z->gesamt - n;
    uint8_t *lin = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!lin) return;
    for (uint32_t i = 0; i < n; i++) lin[i] = z->ring[(ab + i) % RING_N];
    int eocd = -1;
    for (int i = (int)n - 22; i >= 0; i--) if (le32(lin + i) == 0x06054b50) { eocd = i; break; }
    if (eocd >= 0) {
        uint32_t cd_n = le32(lin + eocd + 12), cd_ab = le32(lin + eocd + 16);
        if (cd_ab >= ab && cd_ab + cd_n <= z->gesamt) {
            const uint8_t *p = lin + (cd_ab - ab), *ende = p + cd_n;
            while (p + 46 <= ende && le32(p) == 0x02014b50) {
                int nl = le16(p + 28), xl = le16(p + 30), cl = le16(p + 32), pl;
                const char *name = (const char *)p + 46;
                if (name_ist_platte(name, nl, &pl) && !z->platte) z->platte = pl;
                if (!z->si_fertig && nl == 26 && !memcmp(name, "Metadata/slice_info.config", 26)) {
                    uint32_t c = le32(p + 20), u = le32(p + 24), lok = le32(p + 42);
                    if (lok >= ab && lok + 30 <= z->gesamt && c <= SI_MAX) {
                        const uint8_t *k = lin + (lok - ab);
                        uint32_t d_ab = lok + 30 + le16(k + 26) + le16(k + 28);
                        if (le32(k) == 0x04034b50 && d_ab + c <= z->gesamt) {
                            memcpy(z->si, lin + (d_ab - ab), c);
                            z->si_c = c; z->si_u = u; z->si_meth = le16(p + 10); z->si_n = c; z->si_fertig = true;
                        }
                    }
                }
                p += 46 + nl + xl + cl;
            }
        }
    }
    heap_caps_free(lin);
}

static void slice_info_lesen(druckdatei_t *k, zipstrom_t *z)
{
    if (!z->si_fertig || z->si_u > 32768) return;
    char *xml = heap_caps_malloc(z->si_u + 1, MALLOC_CAP_SPIRAM);
    tinfl_decompressor *dec = heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM);
    bool ok = xml && dec;
    if (ok && z->si_meth == 0 && z->si_c == z->si_u) memcpy(xml, z->si, z->si_u);
    else if (ok && z->si_meth == 8) {
        tinfl_init(dec);
        size_t in_n = z->si_c, out_n = z->si_u;
        tinfl_status st = tinfl_decompress(dec, z->si, &in_n, (mz_uint8 *)xml, (mz_uint8 *)xml, &out_n,
                                           TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
        ok = st == TINFL_STATUS_DONE && out_n == z->si_u;
    } else ok = false;
    if (ok) {
        xml[z->si_u] = 0;
        char w[24];
        if (xml_wert(xml, "prediction", w, sizeof(w))) k->minuten = (atoi(w) + 30) / 60;
        if (xml_wert(xml, "weight", w, sizeof(w))) k->gramm = (int)(strtod(w, NULL) + 0.5);
        if (xml_wert(xml, "index", w, sizeof(w)) && atoi(w) > 0) k->platte = atoi(w);
        for (const char *t = strstr(xml, "<filament "); t; t = strstr(t + 1, "<filament ")) {
            char id[6], typ[8] = "", farbe[12] = "";
            if (!attr(t, "id", id, sizeof(id))) continue;
            int i = atoi(id) - 1;
            if (i < 0 || i >= FILAMENTE_MAX) continue;
            attr(t, "type", typ, sizeof(typ));
            attr(t, "color", farbe, sizeof(farbe));
            snprintf(k->fil_typ[i], sizeof(k->fil_typ[i]), "%s", typ);
            k->fil_farbe[i] = farbe[0] == '#' ? (uint32_t)(strtoul(farbe + 1, NULL, 16) >> (strlen(farbe) > 7 ? 8 : 0)) : 0;
            if (i + 1 > k->n_fil) k->n_fil = i + 1;
        }
    }
    heap_caps_free(xml);
    heap_caps_free(dec);
}

/* false = nicht druckbar (keine geslicte Platte) oder nicht lesbar */
static bool datei_pruefen(ftp_t *f, druckdatei_t *k, zipstrom_t *z)
{
    uint8_t *ring = z->ring, *si = z->si;
    memset(z, 0, sizeof(*z));
    z->ring = ring;
    z->si = si;
    char b[160];
    snprintf(b, sizeof(b), "RETR %s", k->pfad);
    esp_tls_t *dt = ftp_daten(f, b);
    if (!dt) { ESP_LOGW(TAG, "%s: nicht lesbar", k->name); return false; }
    static uint8_t blk[4096];
    bool abgebrochen = false;
    for (;;) {
        int r = esp_tls_conn_read(dt, blk, sizeof(blk));
        if (r <= 0) break;
        strom_futter(z, blk, r);
        if (z->platte && z->si_fertig) { abgebrochen = z->gesamt < k->groesse; break; }
    }
    esp_tls_conn_destroy(dt);
    if (abgebrochen) ftp_sync(f);
    else ftp_antwort(f, NULL, 0);   /* 226 */
    if (!(z->platte && z->si_fertig)) strom_ende(z);
    ESP_LOGI(TAG, "%s: %lu von %lu Byte gelesen, Platte %d, Metadaten %s", k->name, (unsigned long)z->gesamt,
             (unsigned long)k->groesse, z->platte, z->si_fertig ? "ja" : "nein");
    if (!z->platte) return false;
    k->platte = z->platte;
    slice_info_lesen(k, z);
    return true;
}

static void liste_setzen(int status, int anzahl, const druckdatei_t *d)
{
    xSemaphoreTake(sperre, portMAX_DELAY);
    liste.status = status;
    if (d) {
        liste.anzahl = anzahl;
        memcpy(liste.d, d, sizeof(druckdatei_t) * anzahl);
    }
    liste_stand++;
    xSemaphoreGive(sperre);
}

static void dateien_task(void *arg)
{
    (void)arg;
    ftp_t *f = heap_caps_calloc(1, sizeof(ftp_t), MALLOC_CAP_SPIRAM);
    kand = heap_caps_calloc(KANDIDATEN, sizeof(druckdatei_t), MALLOC_CAP_SPIRAM);
    zipstrom_t *z = heap_caps_calloc(1, sizeof(zipstrom_t), MALLOC_CAP_SPIRAM);
    uint8_t *puf = heap_caps_malloc(RING_N + SI_MAX, MALLOC_CAP_SPIRAM);
    if (z && puf) { z->ring = puf; z->si = puf + RING_N; }
    druckdatei_t gut[DATEIEN_MAX];
    int n_gut = 0;
    bool ok = f && kand && z && puf && ftp_anmelden(f);
    n_kand = 0;
    if (ok) ok = ordner_lesen(f, "/");
    if (ok) ordner_lesen(f, "/cache");
    for (int i = 0; ok && i < n_kand && n_gut < DATEIEN_MAX; i++)
        if (datei_pruefen(f, &kand[i], z)) gut[n_gut++] = kand[i];
    if (f && f->tls) { ftp_befehl(f, "QUIT", NULL, 0); ftp_ende(f); }
    ESP_LOGI(TAG, "Dateien: %d druckbar (%s)", n_gut, ok ? "ok" : "Fehler");
    liste_setzen(ok ? 2 : 3, n_gut, ok ? gut : NULL);
    heap_caps_free(puf);
    heap_caps_free(z);
    heap_caps_free(kand);
    kand = NULL;
    heap_caps_free(f);
    liste_laeuft = false;
    vTaskDeleteWithCaps(NULL);
}

void dateien_laden(void)
{
    if (liste_laeuft || !d.eingerichtet || !d.wlan) return;
    liste_laeuft = true;
    liste_setzen(1, 0, NULL);
    /* Stack im PSRAM (interner RAM ist knapp); der Task greift nicht auf Flash/NVS zu */
    if (xTaskCreatePinnedToCoreWithCaps(dateien_task, "dateien", 12288, NULL, 3, NULL, 0, MALLOC_CAP_SPIRAM) != pdPASS) {
        liste_laeuft = false;
        liste_setzen(3, 0, NULL);
    }
}

/* ------------------------------------------------------------------ Druck starten */

static int farbabstand(uint32_t a, uint32_t b)
{
    int dr = (int)(a >> 16 & 255) - (int)(b >> 16 & 255), dg = (int)(a >> 8 & 255) - (int)(b >> 8 & 255),
        db = (int)(a & 255) - (int)(b & 255);
    return dr * dr + dg * dg + db * db;
}

bool drucker_drucken(const druckdatei_t *f, bool nivellieren, bool flow_kali, bool ams, bool zeitraffer)
{
    char param[32], name[48];
    snprintf(param, sizeof(param), "Metadata/plate_%d.gcode", f->platte > 0 ? f->platte : 1);
    snprintf(name, sizeof(name), "%s", f->name);
    size_t l = strlen(name);
    if (l > 4 && !strcasecmp(name + l - 4, ".3mf")) name[l - 4] = 0;
    char url[128];
    snprintf(url, sizeof(url), "ftp://%s", f->pfad);

    cJSON *p, *w = befehl_neu("print", "project_file", &p);
    cJSON_AddStringToObject(p, "param", param);
    cJSON_AddStringToObject(p, "project_id", "0");
    cJSON_AddStringToObject(p, "profile_id", "0");
    cJSON_AddStringToObject(p, "task_id", "0");
    cJSON_AddStringToObject(p, "subtask_id", "0");
    cJSON_AddStringToObject(p, "subtask_name", name);
    cJSON_AddStringToObject(p, "url", url);
    cJSON_AddStringToObject(p, "md5", "");
    cJSON_AddBoolToObject(p, "timelapse", zeitraffer);
    cJSON_AddStringToObject(p, "bed_type", "auto");
    cJSON_AddBoolToObject(p, "bed_levelling", nivellieren);
    cJSON_AddBoolToObject(p, "flow_cali", flow_kali);
    cJSON_AddBoolToObject(p, "vibration_cali", true);
    cJSON_AddBoolToObject(p, "layer_inspect", false);
    cJSON_AddBoolToObject(p, "use_ams", ams && d.ams);
    if (ams && d.ams) {
        /* jedem Filament der Datei den AMS-Slot zuordnen: gleiches Material, ähnlichste Farbe */
        cJSON *m = cJSON_AddArrayToObject(p, "ams_mapping");
        xSemaphoreTake(sperre, portMAX_DELAY);
        for (int i = 0; i < f->n_fil; i++) {
            int best = -1, best_d = 0x7fffffff;
            for (int pass = 0; pass < 2 && best < 0; pass++)
                for (int s = 0; s < 4; s++) {
                    if (!strcmp(d.material[s], "–")) continue;
                    if (pass == 0 && strcasecmp(d.material[s], f->fil_typ[i])) continue;
                    int a = farbabstand(d.farbe[s], f->fil_farbe[i]);
                    if (a < best_d) { best_d = a; best = s; }
                }
            cJSON_AddItemToArray(m, cJSON_CreateNumber(f->fil_typ[i][0] ? (best < 0 ? 0 : best) : -1));
        }
        xSemaphoreGive(sperre);
    }
    return senden(w);
}
