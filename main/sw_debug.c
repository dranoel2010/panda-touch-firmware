/**
 * Debug-Kanal über die USB-Seriell-Konsole (UART0, 115200 Baud):
 *   snap           -> aktuellen Bildschirm rendern und RLE-komprimiert senden
 *   go <aktion>    -> Aktion wie ein Antippen auslösen (z. B. "go nav:menu")
 *   dark 1|0       -> Hintergrundlicht zwangsweise aus / wieder normal
 * Damit lässt sich die Oberfläche prüfen, ohne den Bildschirm einzuschalten.
 * Gegenstück am Mac: tools/sim/device_snap.py
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "bsp/esp-bsp.h"
#include "driver/uart.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_crc.h"
#include "mbedtls/base64.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void sw_app_debug_go(const char *action);
void sw_app_force_dark(bool on);

static void send(const void *d, size_t n) { uart_write_bytes(UART_NUM_0, d, n); }

/* Binärdaten als Base64-Zeilen senden (die USB-Seriell-Strecke verfälscht rohe Binärdaten gelegentlich) */
static uint8_t pend[3 * 64];
static size_t npend;
static uint32_t crc, total;
static void out_flush(bool all)
{
    while (npend >= 3 * 64 || (all && npend)) {
        size_t n = npend >= 3 * 64 ? 3 * 64 : npend;
        char line[2 + 4 * 64 + 2];
        size_t ol = 0;
        mbedtls_base64_encode((unsigned char *)line + 2, sizeof(line) - 3, &ol, pend, n);
        line[0] = 'D';
        line[1] = ' ';
        line[2 + ol] = '\n';
        send(line, 3 + ol);
        memmove(pend, pend + n, npend - n);
        npend -= n;
    }
}
static void out(const void *d, size_t n)
{
    const uint8_t *p = d;
    crc = esp_crc32_le(crc, p, n);
    total += n;
    while (n) {
        size_t k = sizeof(pend) - npend < n ? sizeof(pend) - npend : n;
        memcpy(pend + npend, p, k);
        npend += k;
        p += k;
        n -= k;
        out_flush(false);
    }
}

esp_lcd_panel_handle_t sw_debug_panel;   /* von main.c gesetzt */

/* Pixel (RGB565, Zeilenabstand stride Bytes) RLE + Base64 senden */
static void send_pixels(const uint8_t *data, uint32_t w, uint32_t h, uint32_t stride, uint32_t ms)
{
    char head[64];
    int hl = snprintf(head, sizeof(head), "\nSNAP %lu %lu\n", (unsigned long)w, (unsigned long)h);
    send(head, hl);
    crc = 0;
    total = 0;
    npend = 0;
    uint16_t cur = 0, cnt = 0;
    for (uint32_t y = 0; y < h; y++) {
        const uint16_t *row = (const uint16_t *)(data + y * stride);
        for (uint32_t x = 0; x < w; x++) {
            uint16_t p = row[x];
            if (cnt && p == cur && cnt < 65535) cnt++;
            else {
                if (cnt) { uint16_t pr[2] = { cnt, cur }; out(pr, 4); }
                cur = p;
                cnt = 1;
            }
        }
    }
    uint16_t pr[2] = { cnt, cur };
    out(pr, 4);
    out_flush(true);
    hl = snprintf(head, sizeof(head), "SNAPEND %lu %08lx %lu\n", (unsigned long)total, (unsigned long)crc,
                  (unsigned long)ms);
    send(head, hl);
    uart_wait_tx_done(UART_NUM_0, portMAX_DELAY);
}

static void snap(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);   /* keine Logzeilen mitten in die Bilddaten */
    if (!bsp_display_lock(portMAX_DELAY)) { esp_log_level_set("*", ESP_LOG_INFO); return; }
    int64_t t0 = esp_timer_get_time();
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    int64_t t1 = esp_timer_get_time();
    bsp_display_unlock();
    if (!buf) {
        send("SNAPERR\n", 8);
        esp_log_level_set("*", ESP_LOG_INFO);
        return;
    }
    send_pixels(buf->data, buf->header.w, buf->header.h, buf->header.stride, (uint32_t)((t1 - t0) / 1000));
    lv_draw_buf_destroy(buf);
    esp_log_level_set("*", ESP_LOG_INFO);
}

/* Inhalt des echten Bildspeichers, den der Display-Controller ausliest */
static void fbsnap(int which)
{
    void *fb[2] = { NULL, NULL };
    if (!sw_debug_panel || esp_lcd_rgb_panel_get_frame_buffer(sw_debug_panel, 1, &fb[0]) != ESP_OK || !fb[which & 1]) {
        send("SNAPERR\n", 8);
        return;
    }
    esp_log_level_set("*", ESP_LOG_NONE);
    send_pixels(fb[which & 1], 800, 480, 800 * 2, 0);
    esp_log_level_set("*", ESP_LOG_INFO);
}

/* Dauer der letzten echten Display-Aktualisierungen, aufgeteilt:
 * gesamt / davor (Abgleich der Puffer) / Zeichnen / Übergabe ans Display (inkl. Warten auf VSYNC) */
static int64_t s_t[4];
static uint32_t s_refr_ms[8], s_part[8][3];
static int s_refr_n;
static lv_area_t s_area;
static int32_t s_marks[8][3];
static int64_t s_w0, s_w1;
static void refr_cb(lv_event_t *e)
{
    int64_t now = esp_timer_get_time();
    switch (lv_event_get_code(e)) {
    case LV_EVENT_REFR_START: s_t[0] = s_t[1] = s_t[2] = s_t[3] = now; break;
    case LV_EVENT_RENDER_START: s_t[1] = now; break;
    case LV_EVENT_FLUSH_START: {
        s_t[2] = now;
        const lv_area_t *a = lv_event_get_param(e);
        if (a) s_area = *a;
        break;
    }
    case LV_EVENT_FLUSH_FINISH: s_t[3] = now; break;
    case LV_EVENT_FLUSH_WAIT_START: s_w0 = now; break;
    case LV_EVENT_FLUSH_WAIT_FINISH: s_w1 = now; break;
    default: {
        int k = s_refr_n++ & 7;
        s_refr_ms[k] = (uint32_t)((now - s_t[0]) / 1000);
        s_part[k][0] = (uint32_t)((s_t[1] - s_t[0]) / 1000);
        s_part[k][1] = (uint32_t)((s_t[2] - s_t[1]) / 1000);
        s_part[k][2] = (uint32_t)((s_t[3] - s_t[2]) / 1000);
        extern int64_t sw_t_root, sw_t_sc0, sw_t_sc1;
        (void)sw_t_root;
        s_marks[k][0] = (int32_t)((s_w0 - s_t[1]) / 100) * 1000 + (int32_t)((s_w1 - s_w0) / 100);
        s_marks[k][1] = (int32_t)((sw_t_sc0 - s_t[1]) / 100);
        s_marks[k][2] = (int32_t)((sw_t_sc1 - s_t[1]) / 100);
    }
    }
}

static void debug_task(void *arg)
{
    (void)arg;
    char line[96];
    int n = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(UART_NUM_0, &c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r') continue;
        if (c != '\n') {
            if (n < (int)sizeof(line) - 1) line[n++] = (char)c;
            continue;
        }
        line[n] = 0;
        n = 0;
        if (!strcmp(line, "snap")) snap();
        else if (!strncmp(line, "baud ", 5)) {   /* schneller übertragen; Host stellt danach ebenfalls um */
            send("OK\n", 3);
            uart_wait_tx_done(UART_NUM_0, portMAX_DELAY);
            vTaskDelay(pdMS_TO_TICKS(50));
            uart_set_baudrate(UART_NUM_0, atoi(line + 5));
        }
        else if (!strcmp(line, "fb0")) fbsnap(0);
        else if (!strcmp(line, "fb1")) fbsnap(1);
        else if (!strncmp(line, "prof ", 5)) {   /* Messung: Teile weglassen und alles neu zeichnen */
            extern int sw_prof_skip;
            if (bsp_display_lock(portMAX_DELAY)) {
                sw_prof_skip = atoi(line + 5);
                lv_obj_invalidate(lv_screen_active());
                bsp_display_unlock();
            }
            send("OK\n", 3);
        }
        else if (!strcmp(line, "mem")) {   /* freier Speicher: intern / PSRAM / größter interner Block */
            char b[96];
            int l = snprintf(b, sizeof(b), "MEM %u %u %u\n", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            send(b, l);
        }
        else if (!strcmp(line, "bench")) {   /* PSRAM-Durchsatz: Bildspeicher kopieren/füllen (Bild ist danach kurz falsch) */
            void *fb[2] = { NULL, NULL };
            char b[128];
            int l = 0;
            if (sw_debug_panel && esp_lcd_rgb_panel_get_frame_buffer(sw_debug_panel, 1, &fb[0]) == ESP_OK) {
                const size_t n = 800 * 480 * 2;
                uint8_t *tmp = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
                int64_t t0 = esp_timer_get_time();
                memcpy(tmp, fb[0], n);
                int64_t t1 = esp_timer_get_time();
                memset(tmp, 0x55, n);
                int64_t t2 = esp_timer_get_time();
                volatile uint32_t acc = 0;
                for (size_t i = 0; i < n / 4; i++) acc += ((uint32_t *)fb[0])[i];
                int64_t t3 = esp_timer_get_time();
                free(tmp);
                l = snprintf(b, sizeof(b), "BENCH copy %lld us, fill %lld us, read %lld us (768 KB)\n", t1 - t0, t2 - t1, t3 - t2);
            } else l = snprintf(b, sizeof(b), "BENCH err\n");
            send(b, l);
        }
        else if (!strcmp(line, "perf")) {
            char b[384];
            int l = snprintf(b, sizeof(b), "PERF");
            for (int i = 0; i < 8; i++) {
                int k = (s_refr_n + i) & 7;
                l += snprintf(b + l, sizeof(b) - l, " %lu(%lu/%lu/%lu)", (unsigned long)s_refr_ms[k], (unsigned long)s_part[k][0],
                              (unsigned long)s_part[k][1], (unsigned long)s_part[k][2]);
            }
            for (int i = 4; i < 8; i++) {
                int k = (s_refr_n + i) & 7;
                l += snprintf(b + l, sizeof(b) - l, " [%ld %ld %ld]", (long)s_marks[k][0], (long)s_marks[k][1], (long)s_marks[k][2]);
            }
            l += snprintf(b + l, sizeof(b) - l, " last=%ld,%ld-%ld,%ld", (long)s_area.x1, (long)s_area.y1, (long)s_area.x2, (long)s_area.y2);
            b[l++] = '\n';
            send(b, l);
        }
        else if (!strcmp(line, "dark 1") || !strcmp(line, "dark 0")) {
            if (bsp_display_lock(portMAX_DELAY)) {
                sw_app_force_dark(line[5] == '1');
                bsp_display_unlock();
            }
            send("OK\n", 3);
        }
        else if (!strncmp(line, "go ", 3)) {
            int64_t t0 = esp_timer_get_time();
            if (bsp_display_lock(portMAX_DELAY)) {
                sw_app_debug_go(line + 3);
                bsp_display_unlock();
            }
            char ok[32];
            int l = snprintf(ok, sizeof(ok), "OK %lu\n", (unsigned long)((esp_timer_get_time() - t0) / 1000));
            send(ok, l);
        }
    }
}

void sw_debug_start(void)
{
    if (!uart_is_driver_installed(UART_NUM_0)) {
        esp_err_t e = uart_driver_install(UART_NUM_0, 1024, 4096, 0, NULL, 0);
        if (e != ESP_OK) ESP_LOGE("debug", "UART-Treiber: %s", esp_err_to_name(e));
    }
    if (bsp_display_lock(portMAX_DELAY)) {
        lv_display_t *d = lv_display_get_default();
        lv_display_add_event_cb(d, refr_cb, LV_EVENT_REFR_START, NULL);
        lv_display_add_event_cb(d, refr_cb, LV_EVENT_REFR_READY, NULL);
        lv_display_add_event_cb(d, refr_cb, LV_EVENT_RENDER_START, NULL);
        lv_display_add_event_cb(d, refr_cb, LV_EVENT_FLUSH_START, NULL);
        lv_display_add_event_cb(d, refr_cb, LV_EVENT_FLUSH_FINISH, NULL);
        lv_display_add_event_cb(d, refr_cb, LV_EVENT_FLUSH_WAIT_START, NULL);
        lv_display_add_event_cb(d, refr_cb, LV_EVENT_FLUSH_WAIT_FINISH, NULL);
        bsp_display_unlock();
    }
    if (xTaskCreate(debug_task, "debug", 6144, NULL, 2, NULL) != pdPASS) ESP_LOGE("debug", "Task nicht startbar");
    ESP_LOGI("debug", "intern frei %u (größter Block %u), PSRAM frei %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
