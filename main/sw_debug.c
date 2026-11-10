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
#include "bsp/esp-bsp.h"
#include "driver/uart.h"
#include "esp_crc.h"
#include "mbedtls/base64.h"
#include "esp_log.h"
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

static void snap(void)
{
    esp_log_level_set("*", ESP_LOG_NONE);   /* keine Logzeilen mitten in die Bilddaten */
    if (!bsp_display_lock(portMAX_DELAY)) { esp_log_level_set("*", ESP_LOG_INFO); return; }
    lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
    bsp_display_unlock();
    if (!buf) {
        send("SNAPERR\n", 8);
        esp_log_level_set("*", ESP_LOG_INFO);
        return;
    }
    uint32_t w = buf->header.w, h = buf->header.h, stride = buf->header.stride;
    char head[64];
    int hl = snprintf(head, sizeof(head), "\nSNAP %lu %lu\n", (unsigned long)w, (unsigned long)h);
    send(head, hl);
    crc = 0;
    total = 0;
    npend = 0;
    /* RLE: Paare (Anzahl 1..65535, Pixel) als 2×uint16 little endian */
    uint16_t cur = 0, cnt = 0;
    for (uint32_t y = 0; y < h; y++) {
        const uint16_t *row = (const uint16_t *)(buf->data + y * stride);
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
    hl = snprintf(head, sizeof(head), "SNAPEND %lu %08lx\n", (unsigned long)total, (unsigned long)crc);
    send(head, hl);
    uart_wait_tx_done(UART_NUM_0, portMAX_DELAY);
    lv_draw_buf_destroy(buf);
    esp_log_level_set("*", ESP_LOG_INFO);
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
        else if (!strcmp(line, "dark 1") || !strcmp(line, "dark 0")) {
            if (bsp_display_lock(portMAX_DELAY)) {
                sw_app_force_dark(line[5] == '1');
                bsp_display_unlock();
            }
            send("OK\n", 3);
        }
        else if (!strncmp(line, "go ", 3)) {
            if (bsp_display_lock(portMAX_DELAY)) {
                sw_app_debug_go(line + 3);
                bsp_display_unlock();
            }
            send("OK\n", 3);
        }
    }
}

void sw_debug_start(void)
{
    if (!uart_is_driver_installed(UART_NUM_0)) uart_driver_install(UART_NUM_0, 1024, 4096, 0, NULL, 0);
    xTaskCreate(debug_task, "debug", 6144, NULL, 2, NULL);
}
