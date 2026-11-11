/**
 * Panda-Touch-Firmware – Oberfläche „Soft White“ (Design: design/soft-white/prototype).
 *
 * Startet dunkel (Ruhebildschirm, Hintergrundlicht aus); Antippen weckt das Gerät.
 * Drucker-Anbindung (WLAN + MQTT zum P1S) in drucker.c, Zugangsdaten in main/secrets.h.
 */
#include <stdio.h>
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "bsp/pandatouch.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_ops.h"
#include "sw_app.h"
#include "drucker.h"

void sw_debug_start(void);

#define VERSION "0.3.0"

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

/* Darstellungsart (0 wie die Original-Firmware: ein Bildspeicher, nur fertige Streifen werden
 * hineinkopiert; das hält den PSRAM frei für das Display, sonst verrutscht das Bild):
 *  0 = Streifen: LVGL zeichnet in Streifen im internen RAM und kopiert sie in den sichtbaren Bildspeicher
 *      (neuer Bildschirm baut sich sichtbar von oben nach unten auf)
 *  1 = direkt mit Doppelpuffer: gezeichnet wird im unsichtbaren Bildspeicher, dann sauber umgeschaltet
 *      (kein Reißen/Aufbauen sichtbar) */
#ifndef ANZEIGE_DIREKT
#define ANZEIGE_DIREKT 0
#endif

/* wie bsp_display_new, aber Anzahl Bildspeicher passend zur Darstellungsart */
static esp_err_t panel_neu(esp_lcd_panel_handle_t *ret)
{
    ESP_ERROR_CHECK(bsp_display_brightness_init());
    gpio_config_t io = { .pin_bit_mask = BIT64(BSP_LCD_RST), .mode = GPIO_MODE_OUTPUT };
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(BSP_LCD_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(BSP_LCD_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_lcd_rgb_panel_config_t c = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = 18 * 1000 * 1000,   /* 23 MHz (BSP) braucht mehr PSRAM-Durchsatz als übrig ist */
            .h_res = BSP_LCD_H_RES,
            .v_res = BSP_LCD_V_RES,
            .hsync_pulse_width = 4,
            .hsync_back_porch = 8,
            .hsync_front_porch = 8,
            .vsync_pulse_width = 4,
            .vsync_back_porch = 16,
            .vsync_front_porch = 16,
            .flags.pclk_active_neg = true,
        },
        .data_width = 16,
        .in_color_format = LCD_COLOR_FMT_RGB565,
        .num_fbs = ANZEIGE_DIREKT ? 2 : 1,
        .bounce_buffer_size_px = BSP_LCD_H_RES * CONFIG_BSP_LCD_RGB_BOUNCE_BUF_HEIGHT,
        .hsync_gpio_num = GPIO_NUM_NC,
        .vsync_gpio_num = GPIO_NUM_NC,
        .de_gpio_num = BSP_LCD_DE,
        .pclk_gpio_num = BSP_LCD_PCLK,
        .data_gpio_nums = { BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3, BSP_LCD_DATA4, BSP_LCD_DATA5,
                            BSP_LCD_DATA6, BSP_LCD_DATA7, BSP_LCD_DATA8, BSP_LCD_DATA9, BSP_LCD_DATA10, BSP_LCD_DATA11,
                            BSP_LCD_DATA12, BSP_LCD_DATA13, BSP_LCD_DATA14, BSP_LCD_DATA15 },
        .disp_gpio_num = GPIO_NUM_NC,
        .flags.fb_in_psram = true,
    };
    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&c, ret));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(*ret));
    ESP_ERROR_CHECK(esp_lcd_panel_init(*ret));
    return ESP_OK;
}

/* Display wie im BSP, Darstellungsart siehe oben. */
static lv_display_t *anzeige_starten(void)
{
    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_affinity = 1;   /* Oberfläche auf Kern 1, WLAN/MQTT/TLS laufen auf Kern 0 */
    if (lvgl_port_init(&port_cfg) != ESP_OK) return NULL;
    esp_lcd_panel_handle_t panel = NULL;
    if (panel_neu(&panel) != ESP_OK) return NULL;
    const lvgl_port_display_cfg_t disp_cfg = {
        .panel_handle = panel,
        .buffer_size = ANZEIGE_DIREKT ? BSP_LCD_H_RES * BSP_LCD_V_RES : BSP_LCD_H_RES * 40,
        .double_buffer = !ANZEIGE_DIREKT,
        .hres = BSP_LCD_H_RES,
        .vres = BSP_LCD_V_RES,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .swap_bytes = (BSP_LCD_BIGENDIAN ? true : false),
            .full_refresh = false,
            .direct_mode = ANZEIGE_DIREKT,
        },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = {
        .flags = { .bb_mode = true, .avoid_tearing = ANZEIGE_DIREKT },
    };
    extern esp_lcd_panel_handle_t sw_debug_panel;
    sw_debug_panel = panel;
    lv_display_t *disp = lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg);
    if (disp && bsp_display_indev_init(disp) != ESP_OK) ESP_LOGW(TAG, "Touch nicht verfügbar");
    return disp;
}

/* Hintergrundlicht sanft nachführen (5 % je 10 ms): ein Sprung von 0 auf 85 % zusammen mit dem
 * WLAN-Funk ließ die Versorgung über USB einbrechen; Ausschalten geht sofort */
static esp_timer_handle_t licht_timer;
static volatile int licht_ziel, licht_ist;

static void licht_schritt(void *arg)
{
    (void)arg;
    int z = licht_ziel, i = licht_ist;
    if (i == z) { esp_timer_stop(licht_timer); return; }
    i = i < z ? (i + 5 > z ? z : i + 5) : z;
    licht_ist = i;
    bsp_display_brightness_set(i);
}

static void hintergrundlicht(int prozent)
{
    licht_ziel = prozent;
    if (prozent <= licht_ist || !licht_timer) {
        licht_ist = prozent;
        bsp_display_brightness_set(prozent);
        return;
    }
    if (!esp_timer_is_active(licht_timer)) esp_timer_start_periodic(licht_timer, 10 * 1000);
}

/* Wächter: hängt die Oberfläche (LVGL-Task läuft nicht mehr), nach 30 s neu starten statt
 * dauerhaft eingefroren zu bleiben */
static volatile int64_t lebenszeichen;
static void lvgl_lebt(lv_timer_t *t) { (void)t; lebenszeichen = esp_timer_get_time(); }
static void waechter(void *arg)
{
    (void)arg;
    if (lebenszeichen && esp_timer_get_time() - lebenszeichen > 30 * 1000000LL) {
        ESP_LOGE(TAG, "Oberfläche reagiert seit 30 s nicht – Neustart");
        esp_restart();
    }
}

void app_main(void)
{
    nvs_starten();

    lv_display_t *disp = anzeige_starten();
    assert(disp);
    bsp_display_brightness_set(0);   /* dunkel starten */
    const esp_timer_create_args_t lt = { .callback = licht_schritt, .name = "licht" };
    esp_timer_create(&lt, &licht_timer);

    if (!bsp_display_lock(portMAX_DELAY)) {
        ESP_LOGE(TAG, "Display-Sperre nicht bekommen");
        return;
    }
    sw_app_start(lv_screen_active(), hintergrundlicht, true);
    lebenszeichen = esp_timer_get_time();
    lv_timer_create(lvgl_lebt, 1000, NULL);
    bsp_display_unlock();
    /* WLAN nach der Oberfläche: deren Objekte sollen im schnellen internen RAM liegen */
    drucker_start();
    sw_debug_start();
    const esp_timer_create_args_t wa = { .callback = waechter, .name = "waechter" };
    esp_timer_handle_t wt;
    if (esp_timer_create(&wa, &wt) == ESP_OK) esp_timer_start_periodic(wt, 5 * 1000000);
    ESP_LOGI(TAG, "Bereit, v%s (Soft White)", VERSION);
}
