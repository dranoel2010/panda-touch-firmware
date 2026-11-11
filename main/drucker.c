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

static void material_setzen(char *ziel, const cJSON *tray)
{
    char typ[8] = "";
    text(tray, "tray_type", typ, sizeof(typ));
    snprintf(ziel, 8, "%s", typ[0] ? typ : "–");
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
            d.ams = erste != NULL;
            if (erste) {
                if (zahl(erste, "humidity", &v)) d.feuchte = (int)v;
                const cJSON *t;
                cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(erste, "tray")) {
                    if (!zahl(t, "id", &v) || v < 0 || v > 3) continue;
                    material_setzen(d.material[(int)v], t);
                }
            }
        }
        if (zahl(ams, "tray_now", &v)) {
            int n = (int)v;
            d.slot_aktiv = n >= 0 && n <= 3 ? n : n == 254 ? 4 : -1;
        }
    }
    const cJSON *vt = cJSON_GetObjectItemCaseSensitive(p, "vt_tray");
    if (cJSON_IsObject(vt)) material_setzen(d.material[4], vt);
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
            ok = esp_mqtt_client_publish(mqtt, topic_befehl, s, 0, 1, 0) >= 0;
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
