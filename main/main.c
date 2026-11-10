/**
 * Panda-Touch-Firmware – Oberfläche „Soft White“ (Design: design/soft-white/prototype).
 *
 * Startet dunkel (Ruhebildschirm, Hintergrundlicht aus); Antippen weckt das Gerät.
 * Die Drucker-Anbindung (MQTT) folgt; bis dahin laufen die Demo-Daten des Prototyps.
 */
#include <stdio.h>
#include "bsp/esp-bsp.h"
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

static void hintergrundlicht(int prozent)
{
    bsp_display_brightness_set(prozent);
}

void app_main(void)
{
    nvs_starten();

    lv_display_t *disp = bsp_display_start();
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
