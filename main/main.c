/**
 * Panda-Touch-Firmware – Oberfläche „Soft White“ (Design: design/soft-white/prototype).
 *
 * Startet dunkel (Ruhebildschirm, Hintergrundlicht aus); Antippen weckt das Gerät.
 * Die Drucker-Anbindung (MQTT) folgt; bis dahin laufen die Demo-Daten des Prototyps.
 */
#include <stdio.h>
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "sw_app.h"

void sw_debug_start(void);

#define VERSION "0.2.0"

static const char *TAG = "panda";

static void nvs_starten(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS wird neu angelegt (%s)", esp_err_to_name(err));
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

esp_err_t bsp_display_indev_init(lv_display_t *disp);   /* bsp_touch.c */

/* Display wie im BSP, aber LVGL zeichnet in Streifen im schnellen internen RAM statt direkt in den
 * Bildspeicher im PSRAM (den der Display-Controller ständig ausliest). Das ist um ein Vielfaches schneller. */
static lv_display_t *anzeige_starten(void)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    if (lvgl_port_init(&port_cfg) != ESP_OK) return NULL;
    esp_lcd_panel_handle_t panel = NULL;
    if (bsp_display_new(NULL, &panel, NULL) != ESP_OK) return NULL;
    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = panel,
        .buffer_size = BSP_LCD_H_RES * 60,
        .double_buffer = true,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .swap_bytes = (BSP_LCD_BIGENDIAN ? true : false),
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = { .bb_mode = true, .avoid_tearing = false },
    };
    lv_display_t *disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (disp && bsp_display_indev_init(disp) != ESP_OK) ESP_LOGW(TAG, "Touch nicht verfügbar");
    return disp;
}

static void hintergrundlicht(int prozent)
{
    bsp_display_brightness_set(prozent);
}

void app_main(void)
{
    nvs_starten();

    lv_display_t *disp = anzeige_starten();
    assert(disp);
    bsp_display_brightness_set(0);   /* dunkel starten */

    if (!bsp_display_lock(portMAX_DELAY)) {
        ESP_LOGE(TAG, "Display-Sperre nicht bekommen");
        return;
    }
    sw_app_start(lv_screen_active(), hintergrundlicht, true);
    bsp_display_unlock();
    sw_debug_start();
    ESP_LOGI(TAG, "Bereit, v%s (Soft White)", VERSION);
}
