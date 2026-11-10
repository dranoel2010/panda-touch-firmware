/**
 * Panda-Touch-Firmware – Schritt 1: Display- und Touch-Test.
 * Zeigt Titel + Versionsnummer, einen Knopf, der Berührungen zählt,
 * und einen Helligkeitsregler, dessen Wert im NVS gespeichert wird.
 */
#include <stdio.h>
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#define VERSION "0.1.1"

#define HELL_MIN      1     /* nie ganz schwarz (0), sonst sieht man den Regler nicht mehr */
#define HELL_MAX      100
#define HELL_STANDARD 80

static const char *TAG = "panda";
static lv_obj_t *zaehler_label;
static lv_obj_t *hell_label;
static int tipps = 0;

/* ---------- Helligkeit im NVS merken ---------- */

static void nvs_starten(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Reste der Original-Firmware passen nicht zum neuen Format: neu anlegen */
        ESP_LOGW(TAG, "NVS wird neu angelegt (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

static int helligkeit_laden(void)
{
    nvs_handle_t h;
    uint8_t wert = HELL_STANDARD;
    if (nvs_open("anzeige", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "hell", &wert);
        nvs_close(h);
    }
    if (wert < HELL_MIN) wert = HELL_MIN;
    if (wert > HELL_MAX) wert = HELL_MAX;
    return wert;
}

static void helligkeit_speichern(int wert)
{
    nvs_handle_t h;
    if (nvs_open("anzeige", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "hell", (uint8_t)wert);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ---------- Ereignisse ---------- */

static void knopf_gedrueckt(lv_event_t *e)
{
    (void)e;
    tipps++;
    lv_label_set_text_fmt(zaehler_label, "Touch geht! %d x getippt", tipps);
    ESP_LOGI(TAG, "Tipp %d", tipps);
}

static void regler_bewegt(lv_event_t *e)
{
    lv_obj_t *regler = lv_event_get_target_obj(e);
    int wert = lv_slider_get_value(regler);
    bsp_display_brightness_set(wert);
    lv_label_set_text_fmt(hell_label, "Helligkeit: %d %%", wert);
}

static void regler_losgelassen(lv_event_t *e)
{
    /* erst beim Loslassen speichern, nicht bei jeder Bewegung (schont den Flash) */
    int wert = lv_slider_get_value(lv_event_get_target_obj(e));
    helligkeit_speichern(wert);
    ESP_LOGI(TAG, "Helligkeit gespeichert: %d %%", wert);
}

/* ---------- Start ---------- */

void app_main(void)
{
    nvs_starten();
    int hell = helligkeit_laden();

    lv_display_t *disp = bsp_display_start();
    assert(disp);
    bsp_display_brightness_set(hell);

    if (!bsp_display_lock(portMAX_DELAY)) {
        ESP_LOGE(TAG, "Display-Sperre nicht bekommen");
        return;
    }
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *titel = lv_label_create(scr);
    lv_label_set_text(titel, "Panda Touch - eigene Firmware v" VERSION);
    lv_obj_set_style_text_color(titel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(titel, LV_ALIGN_TOP_MID, 0, 30);

    lv_obj_t *knopf = lv_button_create(scr);
    lv_obj_set_size(knopf, 320, 110);
    lv_obj_align(knopf, LV_ALIGN_CENTER, 0, -50);
    lv_obj_add_event_cb(knopf, knopf_gedrueckt, LV_EVENT_CLICKED, NULL);
    lv_obj_t *knopf_text = lv_label_create(knopf);
    lv_label_set_text(knopf_text, "Tippen zum Testen");
    lv_obj_center(knopf_text);

    zaehler_label = lv_label_create(scr);
    lv_label_set_text(zaehler_label, "Noch nicht getippt");
    lv_obj_set_style_text_color(zaehler_label, lv_color_hex(0xA0E0A0), 0);
    lv_obj_align(zaehler_label, LV_ALIGN_CENTER, 0, 30);

    /* Helligkeitsregler: breit und mit großem Knopf, damit er gut zu treffen ist */
    hell_label = lv_label_create(scr);
    lv_label_set_text_fmt(hell_label, "Helligkeit: %d %%", hell);
    lv_obj_set_style_text_color(hell_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(hell_label, LV_ALIGN_BOTTOM_MID, 0, -95);

    lv_obj_t *regler = lv_slider_create(scr);
    lv_obj_set_size(regler, 600, 24);
    lv_obj_align(regler, LV_ALIGN_BOTTOM_MID, 0, -45);
    lv_slider_set_range(regler, HELL_MIN, HELL_MAX);
    lv_slider_set_value(regler, hell, LV_ANIM_OFF);
    lv_obj_set_style_pad_all(regler, 14, LV_PART_KNOB);           /* Griff größer */
    lv_obj_set_ext_click_area(regler, 30);                         /* Trefferfläche größer */
    lv_obj_add_event_cb(regler, regler_bewegt, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(regler, regler_losgelassen, LV_EVENT_RELEASED, NULL);

    bsp_display_unlock();
    ESP_LOGI(TAG, "Bereit, v%s, Helligkeit %d %%", VERSION, hell);
}
