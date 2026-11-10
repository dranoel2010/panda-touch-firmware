/**
 * Panda-Touch-Firmware – Schritt 1: Display- und Touch-Test.
 * Zeigt Titel + Versionsnummer und einen Knopf, der Berührungen zählt.
 */
#include <stdio.h>
#include "bsp/esp-bsp.h"
#include "esp_log.h"

#define VERSION "0.1.0"

static const char *TAG = "panda";
static lv_obj_t *zaehler_label;
static int tipps = 0;

static void knopf_gedrueckt(lv_event_t *e)
{
    (void)e;
    tipps++;
    lv_label_set_text_fmt(zaehler_label, "Touch geht! %d x getippt", tipps);
    ESP_LOGI(TAG, "Tipp %d", tipps);
}

void app_main(void)
{
    lv_display_t *disp = bsp_display_start();
    assert(disp);
    bsp_display_brightness_set(80);

    if (!bsp_display_lock(portMAX_DELAY)) {
        ESP_LOGE(TAG, "Display-Sperre nicht bekommen");
        return;
    }
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x101418), 0);

    lv_obj_t *titel = lv_label_create(scr);
    lv_label_set_text(titel, "Panda Touch  –  eigene Firmware v" VERSION);
    lv_obj_set_style_text_color(titel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(titel, LV_ALIGN_TOP_MID, 0, 40);

    lv_obj_t *knopf = lv_button_create(scr);
    lv_obj_set_size(knopf, 320, 120);
    lv_obj_center(knopf);
    lv_obj_add_event_cb(knopf, knopf_gedrueckt, LV_EVENT_CLICKED, NULL);
    lv_obj_t *knopf_text = lv_label_create(knopf);
    lv_label_set_text(knopf_text, "Tippen zum Testen");
    lv_obj_center(knopf_text);

    zaehler_label = lv_label_create(scr);
    lv_label_set_text(zaehler_label, "Noch nicht getippt");
    lv_obj_set_style_text_color(zaehler_label, lv_color_hex(0xA0E0A0), 0);
    lv_obj_align(zaehler_label, LV_ALIGN_BOTTOM_MID, 0, -50);

    bsp_display_unlock();
    ESP_LOGI(TAG, "Bereit, v%s", VERSION);
}
