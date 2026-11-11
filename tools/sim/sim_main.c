/* Mac-Simulator: startet dieselbe App-Logik wie das Gerät, führt Aktionen aus und schreibt das Bild als PPM.
 * Aufruf: sim <ausgabe.ppm> [aktion ...]       z. B.  sim menu.ppm nav:menu
 *         sim --screen <name> <ausgabe.ppm>      (einzelne erzeugte Bildschirm-Tabelle) */
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include "lvgl.h"
#include "ui/gen/sw_gen_screens.h"
#include "src/core/lv_obj_scroll_private.h"
#include "ui/sw_app.h"
#include "ui/sw_render.h"

void sw_app_debug_go(const char *action);
static uint16_t fb[800 * 480];

static uint32_t tick(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *px)
{
    (void)a; (void)px;
    lv_display_flush_ready(d);
}

static void write_ppm(const char *path)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n800 480\n255\n");
    for (int i = 0; i < 800 * 480; i++) {
        uint16_t c = fb[i];
        uint8_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
        uint8_t px[3] = { (uint8_t)((r << 3) | (r >> 2)), (uint8_t)((g << 2) | (g >> 4)), (uint8_t)((b << 3) | (b >> 2)) };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static lv_obj_tree_walk_res_t find_scroll(lv_obj_t *o, void *ud)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_SCROLLABLE) && lv_obj_get_scroll_bottom(o) > 0) { *(lv_obj_t **)ud = o; return LV_OBJ_TREE_WALK_END; }
    return LV_OBJ_TREE_WALK_NEXT;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "Aufruf: sim <ausgabe.ppm> [aktion ...] | sim --screen <name> <ausgabe.ppm>\n"); return 1; }
    lv_init();
    lv_tick_set_cb(tick);
    lv_display_t *d = lv_display_create(800, 480);
    lv_display_set_color_format(d, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(d, fb, NULL, sizeof(fb), LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(d, flush);
    if (!strcmp(argv[1], "--screen")) {
        for (int i = 0; i < sw_all_screens_count; i++)
            if (!strcmp(sw_all_screens[i]->name, argv[2])) sw_screen_build(sw_all_screens[i], lv_screen_active());
        lv_refr_now(d);
        write_ppm(argv[3]);
        return 0;
    }
    sw_app_start(lv_screen_active(), NULL, false);
    if (getenv("SW_NOCACHE")) { extern int sw_prof_skip; sw_prof_skip = 8; }
    for (int i = 2; i < argc; i++) {
        if (!strncmp(argv[i], "wait:", 5)) {   /* Zeit vergehen lassen (Timer, Animationen) */
            int ms = atoi(argv[i] + 5);
            for (int k = 0; k < ms / 10; k++) { lv_timer_handler(); usleep(10000); }
            continue;
        }
#ifdef SW_LIVE_SIM
        if (!strncmp(argv[i], "dr:", 3)) {   /* Schein-Drucker umstellen, dann die Oberfläche nachziehen lassen */
            extern void drucker_sim_setzen(const char *a);
            drucker_sim_setzen(argv[i]);
            for (int k = 0; k < 60; k++) { lv_timer_handler(); usleep(10000); }
            continue;
        }
#endif
        sw_app_debug_go(argv[i]);
        lv_timer_handler();
    }
    if (getenv("SW_SCROLLPROF")) {   /* Zeitmessung: n Scroll-Bilder (mit Scrollbild) */
        int n = atoi(getenv("SW_SCROLLPROF"));
        lv_obj_t *c = NULL;
        lv_obj_tree_walk(lv_screen_active(), find_scroll, &c);
        if (!c) { printf("kein Scrollbereich\n"); return 1; }
        lv_obj_send_event(c, LV_EVENT_SCROLL_BEGIN, NULL);
        uint32_t t0 = tick();
        for (int k = 0; k < n; k++) {
            lv_obj_scroll_by_raw(c, 0, (k / 50) % 2 ? 3 : -3);
            lv_obj_send_event(c, LV_EVENT_SCROLL, NULL);
            lv_refr_now(d);
        }
        printf("%.2f ms je Scroll-Bild\n", (tick() - t0) / (double)n);
        return 0;
    }
    if (getenv("SW_PROF")) {   /* Zeitmessung: Bildschirm 30× komplett neu zeichnen */
        extern int sw_prof_skip;
        sw_prof_skip = atoi(getenv("SW_PROF"));
        uint32_t t0 = tick();
        for (int k = 0; k < 30; k++) { lv_obj_invalidate(lv_screen_active()); lv_refr_now(d); }
        printf("%.1f ms je Bild\n", (tick() - t0) / 30.0);
        return 0;
    }
    /* Animationen (z. B. Toast 0,2 s) zu Ende laufen lassen */
    for (int k = 0; k < 40; k++) { lv_timer_handler(); usleep(10000); }
    lv_refr_now(d);
    write_ppm(argv[1]);
    return 0;
}
