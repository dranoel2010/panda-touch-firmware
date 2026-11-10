/**
 * Soft White – App-Logik, 1:1 nach design/soft-white/prototype/app.js.
 *
 * Wie im Prototyp wird bei jeder Zustandsänderung der Bildschirm neu aufgebaut (render) und mit den
 * aktuellen Werten befüllt (patch). Die Bildschirm-Varianten selbst (Druck läuft, pausiert, fertig …)
 * stammen aus den Messungen des Prototyps (gen/), damit jedes Detail exakt dem Design entspricht.
 */
#include "sw_app.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "gen/sw_gen_screens.h"
#include "sw_render.h"
#include "sw_text.h"

/* ------------------------------------------------------------------ Zustand (wie defaults() im Prototyp) */

typedef enum { ST_PRINTING, ST_PAUSED, ST_HEATING, ST_LEVELLING, ST_FINISHED, ST_IDLE, ST_OFFLINE, ST_ERROR, ST_LOADING } status_t;
static const char *const STATUS_NAMES[] = { "printing", "paused", "heating", "levelling", "finished", "idle", "offline", "error", "loading" };

typedef struct { char name[32]; char result[16]; char duration[24]; } hist_t;
typedef struct { const char *name, *time, *material, *weight; int minutes; } file_t;
static const file_t FILES[3] = {
    { "Organizer.3mf", "3 h 54 min", "PLA", "86 g", 234 },
    { "Kabelclip.3mf", "32 min", "PLA", "12 g", 32 },
    { "Pflanztopf.3mf", "2 h 12 min", "PETG", "64 g", 132 },
};
static const char *const OPT_NAMES[4] = { "Bett-Nivellierung", "Flow-Kalibrierung", "AMS nutzen", "Zeitraffer" };

static struct {
    status_t status;
    int progress, remaining;
    char file[32];
    bool light;
    int nozzle, bed, nozzle_now, bed_now;
    char speed[12];
    int fans[3];
    int x, y, z;
    bool homed;
    int step, slot;
    bool ams, drying;
    struct { char material[8]; } slots[5];
    int brightness, timeout_min;
    bool sound, night, power, auto_off, connected, developer, sensor, usb, empty;
    hist_t history[8];
    int n_history;
    int message; /* 0 keine, 1 Filamentzufuhr, 2 Developer Mode */
    bool opt[4];
} s;

static void defaults(void)
{
    memset(&s, 0, sizeof(s));
    s.status = ST_PRINTING;
    s.progress = 64;
    s.remaining = 84;
    strcpy(s.file, "Organizer.3mf");
    s.light = true;
    s.nozzle = s.nozzle_now = 220;
    s.bed = s.bed_now = 55;
    strcpy(s.speed, "Standard");
    s.fans[0] = 70; s.fans[1] = 30; s.fans[2] = 50;
    s.x = 110; s.y = 110; s.z = 38;
    s.step = 10;
    s.ams = true;
    const char *mats[5] = { "PLA", "PETG", "PLA", "ABS", "PLA" };
    for (int i = 0; i < 5; i++) strcpy(s.slots[i].material, mats[i]);
    s.brightness = 85;
    s.timeout_min = 5;
    s.sound = true;
    s.power = true;
    s.connected = true;
    s.developer = true;
    s.sensor = true;
    s.usb = true;
    s.n_history = 2;
    s.history[0] = (hist_t){ "Kabelclip.3mf", "Fertig", "32 min" };
    s.history[1] = (hist_t){ "Pflanztopf.3mf", "Fertig", "2 h 12 min" };
    s.opt[0] = true; s.opt[2] = true;
}

static bool active(void)
{
    return s.status == ST_PRINTING || s.status == ST_PAUSED || s.status == ST_HEATING || s.status == ST_LEVELLING;
}

static void temp_time(char *buf, size_t n)
{
    if (s.remaining >= 60) snprintf(buf, n, "%d h %d min", s.remaining / 60, s.remaining % 60);
    else snprintf(buf, n, "%d min", s.remaining);
}

static void end_time(char *buf, size_t n)
{
    time_t t = time(NULL) + s.remaining * 60;
    struct tm tm;
    localtime_r(&t, &tm);
    snprintf(buf, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

static const char *phase(void)
{
    switch (s.status) {
    case ST_PRINTING: return "Druck läuft";
    case ST_PAUSED: return "Druck pausiert";
    case ST_IDLE: return "Bereit für Neues";
    case ST_HEATING: return "Heizt auf";
    case ST_LEVELLING: return "Nivellierung";
    case ST_FINISHED: return "Druck fertig";
    case ST_OFFLINE: return "Verbindung weg";
    case ST_ERROR: return "Druck angehalten";
    case ST_LOADING: return "Verbinde …";
    }
    return "Bereit";
}

/* ------------------------------------------------------------------ Navigation und Ebenen */

typedef enum { R_HOME, R_MENU, R_CONTROL, R_TEMPS, R_FILAMENT, R_SLOT, R_WIZARD, R_FILES, R_FILE, R_MOVEMENT,
               R_FANS, R_HISTORY, R_MESSAGES, R_CONNECTION, R_SETUP, R_POWER, R_SYSTEM, R_DEMO } route_t;
static const char *const ROUTE_NAMES[] = { "home", "menu", "control", "temperatures", "filament", "slot", "wizard", "files",
                                           "file", "movement", "fans", "history", "messages", "connection", "setup",
                                           "power", "system", "demo" };

static route_t route = R_HOME;
static int file_source;          /* 0 SD-Karte, 1 USB-Stick */
static int file_index, slot_index, wizard_step;
static bool wizard_load = true;  /* laden / entladen */
static lv_obj_t *g_parent;
static sw_screen_t *scr, *ov_modal, *ov_toast, *ov_sleep;
static sw_backlight_cb_t g_backlight;
typedef void (*fn_t)(void);
static fn_t modal_action;
static uint32_t async_token;
static lv_timer_t *toast_timer, *idle_timer, *busy_timer;
static fn_t busy_cb;

static void render(void);
static void go(route_t r) { route = r; render(); }
static void set_select(const char *field, int index);
static int timeout_index(void);
static int wifi_index;

static int key(const char *k) { return scr ? sw_key(scr->def, k) : -1; }
static void set_key_text(const char *k, const char *txt) { int i = key(k); if (i >= 0) sw_set_text(scr, i, txt); }

static void set_key_textf(const char *k, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void set_key_textf(const char *k, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    set_key_text(k, buf);
}

static void select_action(const char *action, bool sel)
{
    int i = key(action);
    if (i >= 0) sw_set_selected(scr, i, sel);
}

/* Text innerhalb eines Aktions-Containers (z. B. Beschriftung eines Knopfs) */
static void set_action_text(const char *action, const char *txt)
{
    int a = key(action);
    if (a < 0) return;
    for (int i = 0; i < scr->def->count; i++)
        if (scr->def->nodes[i].parent == a && scr->def->nodes[i].kind == SW_N_LABEL) { sw_set_text(scr, i, txt); return; }
}

static bool g_force_dark;   /* Debug-Kanal: Licht bleibt aus (nachts testen) */
void sw_app_force_dark(bool on);

static void apply_backlight(void)
{
    if (!g_backlight) return;
    static int last = -1;
    int v = (ov_sleep || g_force_dark) ? 0 : (s.night ? 20 : s.brightness);
    if (v != last) {
        last = v;
        g_backlight(v);
    }
}

/* ------------------------------------------------------------------ Toast */

static void toast_hide(lv_timer_t *t)
{
    (void)t;
    if (ov_toast) lv_obj_add_flag(ov_toast->root, LV_OBJ_FLAG_HIDDEN);
}

static void anim_opa(void *o, int32_t v) { lv_obj_set_style_opa_layered(o, v, 0); }
static void anim_ty(void *o, int32_t v) { lv_obj_set_style_translate_y(o, v, 0); }

static void toast(const char *text)
{
    const sw_screen_def_t *def = &sw_scr_toast;
    int box = sw_key(def, "box:show");
    if (!ov_toast) ov_toast = sw_overlay_build(def, g_parent, box);
    lv_obj_remove_flag(ov_toast->objs[box], LV_OBJ_FLAG_CLICKABLE);   /* pointer-events: none */
    lv_obj_remove_flag(ov_toast->root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(ov_toast->root);
    int lbl = sw_key(def, "text:show");
    sw_set_text(ov_toast, lbl, text);
    const sw_node_t *ln = &def->nodes[lbl];
    int32_t w = (sw_text_width_q4(ln->font, text, ln->ls_q4) + 15) / 16 + 48;   /* padding 16px 24px */
    sw_set_width_centered(ov_toast, box, w);
    /* #toast: opacity 0 -> 1, translateY 14px -> 0 in .2s */
    lv_obj_t *o = ov_toast->objs[box];
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_duration(&a, 200);
    lv_anim_set_exec_cb(&a, anim_opa);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_start(&a);
    lv_anim_set_exec_cb(&a, anim_ty);
    lv_anim_set_values(&a, 14, 0);
    lv_anim_start(&a);
    if (toast_timer) lv_timer_reset(toast_timer);
    else {
        toast_timer = lv_timer_create(toast_hide, 3000, NULL);
    }
    lv_timer_set_repeat_count(toast_timer, 1);
    lv_timer_set_auto_delete(toast_timer, false);
    lv_timer_resume(toast_timer);
}

/* ------------------------------------------------------------------ Dialoge */

static void close_modal(void)
{
    async_token++;
    if (ov_modal) { sw_screen_free(ov_modal); ov_modal = NULL; }
    modal_action = NULL;
}

static void modal(const sw_screen_def_t *def, fn_t action)
{
    close_modal();
    ov_modal = sw_overlay_build(def, g_parent, sw_key(def, "box:modal-shade"));
    modal_action = action;
    if (ov_toast) lv_obj_move_foreground(ov_toast->root);
    if (ov_sleep) lv_obj_move_foreground(ov_sleep->root);
}

static uint32_t busy_token;
static void busy_done(lv_timer_t *t)
{
    uint32_t tok = (uint32_t)(uintptr_t)lv_timer_get_user_data(t);
    busy_timer = NULL;
    if (tok != async_token) return;
    fn_t cb = busy_cb;
    close_modal();
    if (cb) cb();
}

static void busy(const sw_screen_def_t *def, fn_t cb)
{
    modal(def, NULL);
    busy_cb = cb;
    busy_token = async_token;
    lv_timer_t *t = lv_timer_create(busy_done, 1300, (void *)(uintptr_t)busy_token);
    lv_timer_set_repeat_count(t, 1);
    busy_timer = t;
}

/* ------------------------------------------------------------------ Ruhebildschirm */

static void sleep_now(void)
{
    if (ov_sleep) return;
    const sw_screen_def_t *def = &sw_scr_sleep;
    ov_sleep = sw_overlay_build(def, g_parent, sw_key(def, "wake"));
    apply_backlight();
}

static void idle_fire(lv_timer_t *t) { (void)t; sleep_now(); }

static void reset_idle(void)
{
    if (idle_timer) { lv_timer_delete(idle_timer); idle_timer = NULL; }
    if (s.timeout_min > 0) {
        idle_timer = lv_timer_create(idle_fire, (uint32_t)s.timeout_min * 60000u, NULL);
        lv_timer_set_repeat_count(idle_timer, 1);
    }
}

/* ------------------------------------------------------------------ Bildschirm wählen und befüllen */

static const sw_screen_def_t *pick_def(void)
{
    switch (route) {
    case R_HOME:
        if (!s.connected) return &sw_scr_home_offline;
        switch (s.status) {
        case ST_PAUSED: return &sw_scr_home_paused;
        case ST_FINISHED: return &sw_scr_home_finished;
        case ST_IDLE: return &sw_scr_home_idle;
        case ST_OFFLINE: return &sw_scr_home_offline;
        case ST_ERROR: return &sw_scr_home_error;
        case ST_HEATING: return &sw_scr_home_heating;
        case ST_LEVELLING: return &sw_scr_home_levelling;
        case ST_LOADING: return &sw_scr_home_loading;
        default: return &sw_scr_home;
        }
    case R_MENU: return &sw_scr_menu;
    case R_CONTROL: return s.status == ST_PAUSED ? &sw_scr_control_paused : &sw_scr_control;
    case R_TEMPS: return &sw_scr_temperatures;
    case R_FILAMENT: return s.ams ? &sw_scr_filament : &sw_scr_filament_noams;
    case R_SLOT: return slot_index == 4 ? &sw_scr_slot_ext : &sw_scr_slot;
    case R_WIZARD: {
        static const sw_screen_def_t *const load[4] = { &sw_scr_wizard, &sw_scr_wizard_1, &sw_scr_wizard_2, &sw_scr_wizard_3 };
        static const sw_screen_def_t *const unload[4] = { &sw_scr_wizard_out0, &sw_scr_wizard_out1, &sw_scr_wizard_out2,
                                                          &sw_scr_wizard_out3 };
        return (wizard_load ? load : unload)[wizard_step & 3];
    }
    case R_FILES: {
        bool available = file_source == 0 || s.usb;
        if (!s.connected && file_source == 0) return &sw_scr_files_offline;
        if (!available) return &sw_scr_files_usb_empty;
        if (s.empty) return &sw_scr_files_empty;
        return file_source == 0 ? &sw_scr_files : &sw_scr_files_usb;
    }
    case R_FILE: return file_index == 1 ? &sw_scr_file_1 : file_index == 2 ? &sw_scr_file_2 : &sw_scr_file;
    case R_MOVEMENT: return &sw_scr_movement;
    case R_FANS: return &sw_scr_fans;
    case R_HISTORY: return s.n_history ? &sw_scr_history : &sw_scr_history_empty;
    case R_MESSAGES: return s.message == 1 ? &sw_scr_messages_error : s.message == 2 ? &sw_scr_messages_dev : &sw_scr_messages;
    case R_CONNECTION: return s.connected ? &sw_scr_connection : &sw_scr_connection_offline;
    case R_SETUP: return &sw_scr_setup;
    case R_POWER: return &sw_scr_power;
    case R_SYSTEM: return &sw_scr_system;
    case R_DEMO: return &sw_scr_demo;
    }
    return &sw_scr_home;
}

static void patch_home(void)
{
    bool done = s.status == ST_FINISHED, idle = s.status == ST_IDLE;
    char buf[32];
    set_key_textf("text:rail/strong", "%d %%", s.progress);
    int fill = key("box:progress-fill");
    if (fill >= 0) {
        int h = (int)lround(270.0 * s.progress / 100.0);
        sw_set_hidden(scr, fill, h <= 0);
        if (h > 0) sw_set_height_from_bottom(scr, fill, h);
    }
    set_key_text("text:phase", phase());
    if (!done && !idle) {
        temp_time(buf, sizeof(buf));
        set_key_text("text:remaining/strong", buf);
    }
    if (!done) {
        if (active() && s.status != ST_PAUSED && s.connected) end_time(buf, sizeof(buf));
        else strcpy(buf, "–");
        set_key_text("text:finish/strong", buf);
    }
    if (!idle) {
        set_key_text("text:job-title", s.file);
        set_key_textf("text:job-detail", "Schicht %d / 300", (int)lround(s.progress * 3.0));
    }
    set_key_textf("text:home-temps/button", "Düse %d °C", s.nozzle_now);
    set_key_textf("text:home-temps/button#2", "Bett %d °C", s.bed_now);
    set_key_textf("text:connection", "P1S · %s", s.connected ? "Verbunden" : "Offline");
    int dot = key("box:light-dot");
    if (dot >= 0 && !s.light) {
        const sw_screen_def_t *off = &sw_scr_home_lightoff;
        sw_set_box_style(scr, dot, off->nodes[sw_key(off, "box:light-dot")].st);
    }
}

static void patch(void)
{
    char buf[64];
    switch (route) {
    case R_HOME: patch_home(); break;
    case R_CONTROL:
        set_key_text("text:label", phase());
        set_key_textf("text:big-number", "%d %%", s.progress);
        select_action("speed:Leise", !strcmp(s.speed, "Leise"));
        select_action("speed:Standard", !strcmp(s.speed, "Standard"));
        select_action("speed:Sport", !strcmp(s.speed, "Sport"));
        select_action("speed:Ludicrous", !strcmp(s.speed, "Ludicrous"));
        if (key("pause") >= 0) sw_set_disabled(scr, key("pause"), !active() || !s.connected);
        if (key("cancel") >= 0) sw_set_disabled(scr, key("cancel"), !active());
        break;
    case R_TEMPS:
        set_key_textf("text:big-number", "%d°", s.nozzle);
        set_key_textf("text:big-number#2", "%d°", s.bed);
        set_key_textf("text:muted", "· Ist %d °C", s.nozzle_now);
        set_key_textf("text:muted#2", "· Ist %d °C", s.bed_now);
        set_key_text("text:notice", s.sensor ? "Bauraum 36 °C · Raum 23 °C · Luftfeuchte 42 %"
                                             : "Bauraum 36 °C · Kein Raumsensor angeschlossen");
        break;
    case R_FILAMENT:
        set_action_text("dry", s.drying ? "Trocknung stoppen" : "Trocknung starten");
        select_action("dry", s.drying);
        set_key_text("text:sub", s.drying ? "Trocknet · 4 h" : "4 Slots + extern");
        for (int i = 0; i < 4; i++) {
            char k[32];
            snprintf(k, sizeof(k), i ? "text:slot/small#%d" : "text:slot/small", i + 1);
            set_key_text(k, s.slot == i ? "Aktiv" : "Bereit");
            snprintf(k, sizeof(k), i ? "text:slot/strong#%d" : "text:slot/strong", i + 1);
            set_key_textf(k, "Slot %d · %s", i + 1, s.slots[i].material);
        }
        {   /* aktiver Punkt wandert in den aktiven Slot */
            int dot = key("box:active-dot");
            if (dot >= 0) {
                char a[12];
                snprintf(a, sizeof(a), "slot:%d", s.slot);
                int target = key(a);
                sw_set_hidden(scr, dot, s.slot < 0 || s.slot > 3);
                if (target >= 0 && s.slot >= 0 && s.slot <= 3) lv_obj_set_parent(sw_obj(scr, dot), sw_obj(scr, target));
            }
        }
        snprintf(buf, sizeof(buf), "%s · %s", s.slots[4].material, s.slot == 4 ? "Aktiv" : "Bereit");
        set_key_text("text:list-item/div/small", buf);
        break;
    case R_SLOT:
        if (slot_index < 4) set_key_textf("text:page-header/h2", "AMS · Slot %d", slot_index + 1);
        set_key_text("text:big-number", s.slots[slot_index].material);
        set_key_text("text:label", s.slot == slot_index ? "Aktives Filament" : "Verfügbar");
        break;
    case R_FILES:
        select_action("source:SD-Karte", file_source == 0);
        select_action("source:USB-Stick", file_source == 1);
        break;
    case R_FILE:
        for (int i = 0; i < 4; i++) {
            char a[48];
            snprintf(a, sizeof(a), "toggle:option:%s", OPT_NAMES[i]);
            select_action(a, s.opt[i]);
        }
        break;
    case R_MOVEMENT:
        set_key_text("text:row/span", s.homed ? "Referenziert" : "Nicht referenziert");
        select_action("step:1", s.step == 1);
        select_action("step:10", s.step == 10);
        select_action("step:50", s.step == 50);
        set_key_textf("text:label", "X %d · Y %d · Z %d mm", s.x, s.y, s.z);
        break;
    case R_FANS:
        select_action("toggle:light", s.light);
        for (int i = 0; i < 3; i++) {
            char k[32];
            snprintf(k, sizeof(k), "field:fan:%d", i);
            if (key(k) >= 0) sw_range_set(scr, key(k), s.fans[i]);
            snprintf(k, sizeof(k), i ? "text:row/strong#%d" : "text:row/strong", i + 1);
            set_key_textf(k, "%d %%", s.fans[i]);
        }
        break;
    case R_HISTORY:
        for (int i = 0; i < 2 && i < s.n_history; i++) {
            char k[40];
            snprintf(k, sizeof(k), i ? "text:list-item/div/strong#%d" : "text:list-item/div/strong", i + 1);
            set_key_text(k, s.history[i].name);
            snprintf(k, sizeof(k), i ? "text:list-item/div/small#%d" : "text:list-item/div/small", i + 1);
            snprintf(buf, sizeof(buf), "%s · %s", s.history[i].result, s.history[i].duration);
            set_key_text(k, buf);
        }
        break;
    case R_CONNECTION:
        set_select("wifi", wifi_index);
        set_action_text("toggle:developer", s.developer ? "Developer Mode: an" : "Developer Mode: aus");
        select_action("toggle:developer", s.developer);
        break;
    case R_SETUP:
        set_select("wifi", wifi_index);
        break;
    case R_POWER:
        set_key_text("text:big-number", s.power ? "Eingeschaltet" : "Ausgeschaltet");
        select_action("power-switch", s.power);
        select_action("toggle:autoOff", s.auto_off);
        break;
    case R_SYSTEM:
        set_key_textf("text:row/strong", "%d %%", s.brightness);
        if (key("field:brightness") >= 0) sw_range_set(scr, key("field:brightness"), s.brightness);
        set_select("timeout", timeout_index());
        select_action("toggle:sound", s.sound);
        select_action("toggle:night", s.night);
        break;
    case R_DEMO:
        for (int i = 0; i < 9; i++) {
            char a[24];
            snprintf(a, sizeof(a), "state:%s", STATUS_NAMES[i]);
            select_action(a, (int)s.status == i);
        }
        select_action("toggle:ams", s.ams);
        select_action("toggle:usb", s.usb);
        select_action("toggle:empty", s.empty);
        select_action("toggle:sensor", s.sensor);
        break;
    default: break;
    }
}

static void render(void)
{
    const sw_screen_def_t *def = pick_def();
    sw_screen_t *old = scr;
    scr = sw_screen_build(def, g_parent);
    if (old) sw_screen_free(old);
    if (ov_modal) lv_obj_move_foreground(ov_modal->root);
    if (ov_toast) lv_obj_move_foreground(ov_toast->root);
    if (ov_sleep) lv_obj_move_foreground(ov_sleep->root);
    patch();
    apply_backlight();
}

/* ------------------------------------------------------------------ Formularfelder */

static const int TIMEOUTS[4] = { 0, 1, 5, 15 };

/* i-ter Eintrag einer Auswahlliste ("a\nb\nc") */
static int option_count(const char *opts)
{
    int n = 1;
    for (const char *p = opts; *p; p++) n += *p == '\n';
    return n;
}
static void option_text(const char *opts, int i, char *buf, size_t n)
{
    const char *p = opts;
    while (i-- > 0 && (p = strchr(p, '\n'))) p++;
    if (!p) { buf[0] = 0; return; }
    size_t l = strcspn(p, "\n");
    if (l >= n) l = n - 1;
    memcpy(buf, p, l);
    buf[l] = 0;
}

static int timeout_index(void)
{
    for (int i = 0; i < 4; i++) if (TIMEOUTS[i] == s.timeout_min) return i;
    return 2;
}

static void set_select(const char *field, int index)
{
    char k[48], txt[48];
    snprintf(k, sizeof(k), "field:%s", field);
    int i = key(k);
    if (i < 0 || !scr->def->nodes[i].options) return;
    option_text(scr->def->nodes[i].options, index, txt, sizeof(txt));
    snprintf(k, sizeof(k), "text:field:%s", field);
    set_key_text(k, txt);
}

static void select_next(const char *field)
{
    char k[48];
    snprintf(k, sizeof(k), "field:%s", field);
    int i = key(k);
    if (i < 0 || !scr->def->nodes[i].options) return;
    const char *opts = scr->def->nodes[i].options;
    int n = option_count(opts);
    if (!strcmp(field, "timeout")) {
        s.timeout_min = TIMEOUTS[(timeout_index() + 1) % 4];
        set_select(field, timeout_index());
        reset_idle();
    } else if (!strcmp(field, "wifi")) {
        wifi_index = (wifi_index + 1) % n;
        set_select(field, wifi_index);
    } else if (!strcmp(field, "slot-material")) {
        char cur[8], nxt[8];
        int idx = 0;
        for (int j = 0; j < n; j++) {
            option_text(opts, j, cur, sizeof(cur));
            if (!strcmp(cur, s.slots[slot_index].material)) idx = j;
        }
        option_text(opts, (idx + 1) % n, nxt, sizeof(nxt));
        snprintf(s.slots[slot_index].material, sizeof(s.slots[slot_index].material), "%s", nxt);
        render();
    }
}

static void on_field(const char *field, int32_t v, const char *text)
{
    (void)text;
    reset_idle();
    if (!strcmp(field, "brightness")) {
        s.brightness = v;
        set_key_textf("text:row/strong", "%d %%", (int)v);
        apply_backlight();
    } else if (!strncmp(field, "fan:", 4)) {
        int i = atoi(field + 4);
        if (!s.connected) { toast("Drucker offline"); render(); return; }
        s.fans[i] = v;
        char k[32];
        snprintf(k, sizeof(k), i ? "text:row/strong#%d" : "text:row/strong", i + 1);
        set_key_textf(k, "%d %%", (int)v);
    }
}

/* ------------------------------------------------------------------ Aktionen (dispatch) */

static bool need_connection(void)
{
    if (s.connected && s.power) return true;
    toast("Drucker offline. Bitte zuerst verbinden.");
    return false;
}

static void finish(void)
{
    s.status = ST_FINISHED;
    s.progress = 100;
    s.remaining = 0;
    if (s.n_history < 8) {
        memmove(&s.history[1], &s.history[0], sizeof(hist_t) * s.n_history);
        s.n_history++;
    }
    s.history[0] = (hist_t){ "", "Fertig", "3 h 54 min" };
    snprintf(s.history[0].name, sizeof(s.history[0].name), "%s", s.file);
    s.nozzle = 0;
    s.bed = 0;
    render();
}

static void history_add(const char *result, const char *duration)
{
    if (s.n_history < 8) {
        memmove(&s.history[1], &s.history[0], sizeof(hist_t) * s.n_history);
        s.n_history++;
    }
    snprintf(s.history[0].name, sizeof(s.history[0].name), "%s", s.file);
    snprintf(s.history[0].result, sizeof(s.history[0].result), "%s", result);
    snprintf(s.history[0].duration, sizeof(s.history[0].duration), "%s", duration);
}

static void do_pause(void)
{
    if (!need_connection() || !active()) return;
    s.status = s.status == ST_PAUSED ? ST_PRINTING : ST_PAUSED;
    render();
    toast(s.status == ST_PAUSED ? "Druck pausiert" : "Druck wird fortgesetzt");
}

static void perform_print(void)
{
    snprintf(s.file, sizeof(s.file), "%s", FILES[file_index].name);
    s.remaining = FILES[file_index].minutes;
    s.progress = 0;
    s.status = ST_HEATING;
    s.nozzle = 220;
    s.bed = 55;
    go(R_HOME);
    toast("Druck gestartet · Düse und Bett heizen auf");
}
static void replace_print(void)
{
    char d[24];
    temp_time(d, sizeof(d));
    history_add("Abgebrochen", d);
    perform_print();
}

static void start_print(void)
{
    if (!need_connection()) return;
    if (active()) modal(&sw_scr_modal_replace, replace_print);
    else perform_print();
}

static char move_axis;
static int move_dir;
static void do_move(void)
{
    int *v = move_axis == 'x' ? &s.x : move_axis == 'y' ? &s.y : &s.z;
    *v += s.step * move_dir;
    if (*v < 0) *v = 0;
    if (*v > 256) *v = 256;
    render();
    char buf[32];
    snprintf(buf, sizeof(buf), "%c → %d mm", move_axis - 32, *v);
    toast(buf);
}

static void movement_action(char axis, int dir)
{
    if (!need_connection()) return;
    if (!s.homed) { toast("Bitte zuerst die Achsen referenzieren."); return; }
    move_axis = axis;
    move_dir = dir;
    if (active()) modal(&sw_scr_modal_move, do_move);
    else do_move();
}

static void homed_done(void)
{
    s.homed = true;
    s.x = s.y = s.z = 0;
    render();
    toast("Alle Achsen referenziert");
}
static void home_axes_busy(void) { busy(&sw_scr_busy_homeaxes, homed_done); }

static int extrude_dir;
static void do_extrude(void) { toast(extrude_dir > 0 ? "5 mm Filament gefördert" : "5 mm Filament zurückgezogen"); }

static void cancel_confirmed(void)
{
    history_add("Abgebrochen", "Teilweise gedruckt");
    s.status = ST_IDLE;
    s.progress = 0;
    s.remaining = 0;
    s.nozzle = s.bed = 0;
    go(R_HOME);
    toast("Druck abgebrochen");
}

static void heat_off(void)
{
    s.nozzle = s.bed = 0;
    render();
    toast("Heizungen ausgeschaltet");
}

static void power_toggle(void)
{
    s.power = !s.power;
    s.connected = s.power;
    if (!s.power) {
        s.status = ST_OFFLINE;
        s.nozzle = s.bed = 0;
    } else s.status = ST_IDLE;
    render();
    toast(s.power ? "Steckdose eingeschaltet" : "Steckdose ausgeschaltet");
}

static void wizard_to_control(void) { go(R_CONTROL); }
static void wizard_advance(void) { wizard_step++; go(R_WIZARD); }

static void reconnect_done(void)
{
    if (!s.developer) {
        s.connected = false;
        s.message = 2;
        s.status = ST_OFFLINE;
        render();
        toast("Developer Mode am Drucker aktivieren.");
    } else if (!s.power) {
        s.connected = false;
        render();
        toast("Drucker-Steckdose ist ausgeschaltet.");
    } else {
        s.connected = true;
        if (s.status == ST_OFFLINE) s.status = s.progress > 0 ? ST_PAUSED : ST_IDLE;
        render();
        toast("Verbindung erfolgreich getestet");
    }
}

static void wifi_done(void)
{
    go(R_CONNECTION);
    toast("WLAN bereit · jetzt Druckerdaten prüfen");
}

static void restart_done(void) { go(R_HOME); }
static void restart_busy(void) { busy(&sw_scr_busy_restart, restart_done); }
static void update_done(void) { toast("Demo 1.0 · Du bist auf dem aktuellen Stand"); }

static void factory_done(void)
{
    defaults();
    go(R_HOME);
    toast("Demo zurückgesetzt");
}

static lv_timer_t *loading_timer;
static void loading_done(lv_timer_t *t)
{
    uint32_t tok = (uint32_t)(uintptr_t)lv_timer_get_user_data(t);
    loading_timer = NULL;
    if (tok == async_token && s.status == ST_LOADING) {
        s.status = ST_PRINTING;
        s.connected = true;
        render();
    }
}

static void state_change(status_t v)
{
    s.connected = v != ST_OFFLINE;
    s.power = true;
    s.status = v;
    if (v == ST_FINISHED) {
        finish();
        go(R_HOME);
        return;
    }
    if (v == ST_IDLE) {
        s.progress = 0;
        s.remaining = 0;
    } else {
        s.progress = 64;
        s.remaining = 84;
    }
    if (v == ST_ERROR) s.message = 1;
    if (v == ST_HEATING || v == ST_LEVELLING) s.progress = 0;
    go(R_HOME);
    if (v == ST_LOADING) {
        uint32_t tok = ++async_token;
        loading_timer = lv_timer_create(loading_done, 1800, (void *)(uintptr_t)tok);
        lv_timer_set_repeat_count(loading_timer, 1);
    }
}

static void dispatch(const char *action)
{
    reset_idle();
    if (!strncmp(action, "nav:", 4)) {
        for (int i = 0; i < (int)(sizeof(ROUTE_NAMES) / sizeof(*ROUTE_NAMES)); i++)
            if (!strcmp(action + 4, ROUTE_NAMES[i])) { go((route_t)i); return; }
        return;
    }
    if (!strcmp(action, "modal-close")) { close_modal(); return; }
    if (!strcmp(action, "modal-confirm")) {
        fn_t fn = modal_action;
        close_modal();
        if (fn) fn();
        return;
    }
    if (!strncmp(action, "toggle:", 7)) {
        const char *k = action + 7;
        if (!strcmp(k, "light") && !need_connection()) return;
        if (!strncmp(k, "option:", 7)) {
            for (int i = 0; i < 4; i++) if (!strcmp(k + 7, OPT_NAMES[i])) s.opt[i] = !s.opt[i];
        } else if (!strcmp(k, "light")) s.light = !s.light;
        else if (!strcmp(k, "ams")) s.ams = !s.ams;
        else if (!strcmp(k, "usb")) s.usb = !s.usb;
        else if (!strcmp(k, "empty")) s.empty = !s.empty;
        else if (!strcmp(k, "sensor")) s.sensor = !s.sensor;
        else if (!strcmp(k, "sound")) s.sound = !s.sound;
        else if (!strcmp(k, "night")) s.night = !s.night;
        else if (!strcmp(k, "autoOff")) s.auto_off = !s.auto_off;
        else if (!strcmp(k, "developer")) s.developer = !s.developer;
        render();
        if (!strcmp(k, "light")) toast(s.light ? "Licht eingeschaltet" : "Licht ausgeschaltet");
        return;
    }
    if (!strncmp(action, "select:", 7)) { select_next(action + 7); return; }
    if (!strcmp(action, "pause")) { do_pause(); return; }
    if (!strcmp(action, "ack-finish")) { s.status = ST_IDLE; s.progress = 0; render(); return; }
    if (!strcmp(action, "cancel")) { modal(&sw_scr_modal_cancel, cancel_confirmed); return; }
    if (!strncmp(action, "speed:", 6)) {
        if (!need_connection()) return;
        snprintf(s.speed, sizeof(s.speed), "%s", action + 6);
        render();
        char buf[48];
        snprintf(buf, sizeof(buf), "Geschwindigkeit: %s", s.speed);
        toast(buf);
        return;
    }
    if (!strncmp(action, "temp:", 5)) {
        if (!need_connection()) return;
        bool bed = !strncmp(action + 5, "bed", 3);
        int delta = atoi(strrchr(action, ':') + 1);
        int *v = bed ? &s.bed : &s.nozzle;
        *v += delta;
        if (*v < 0) *v = 0;
        if (*v > (bed ? 110 : 300)) *v = bed ? 110 : 300;
        render();
        return;
    }
    if (!strncmp(action, "preset:", 7)) {
        if (!need_connection()) return;
        const char *p = action + 7;
        if (!strcmp(p, "PLA")) { s.nozzle = 220; s.bed = 55; }
        else if (!strcmp(p, "PETG")) { s.nozzle = 250; s.bed = 75; }
        else { s.nozzle = 260; s.bed = 100; }
        render();
        char buf[48];
        snprintf(buf, sizeof(buf), "%s-Vorheizen aktiviert", p);
        toast(buf);
        return;
    }
    if (!strcmp(action, "temp-off")) {
        if (!need_connection()) return;
        if (active()) modal(&sw_scr_modal_tempoff, heat_off);
        else heat_off();
        return;
    }
    if (!strcmp(action, "dry")) {
        if (!need_connection()) return;
        s.drying = !s.drying;
        render();
        toast(s.drying ? "AMS-Trocknung simuliert gestartet" : "Trocknung gestoppt");
        return;
    }
    if (!strncmp(action, "slot:", 5)) { slot_index = atoi(action + 5); go(R_SLOT); return; }
    if (!strncmp(action, "wizard:", 7)) {
        if (!need_connection()) return;
        if (active()) { modal(&sw_scr_modal_wizard, wizard_to_control); return; }
        wizard_load = !strcmp(action + 7, "laden");
        wizard_step = 0;
        go(R_WIZARD);
        return;
    }
    if (!strcmp(action, "wizard-next")) {
        if (wizard_step == 3) {
            s.slot = wizard_load ? slot_index : -1;
            go(R_FILAMENT);
            toast(wizard_load ? "Filament geladen" : "Filament entladen");
        } else if (wizard_step == 1) busy(&sw_scr_busy_filament, wizard_advance);
        else { wizard_step++; render(); }
        return;
    }
    if (!strncmp(action, "source:", 7)) { file_source = !strcmp(action + 7, "USB-Stick"); render(); return; }
    if (!strncmp(action, "file:", 5)) { file_index = atoi(action + 5); go(R_FILE); return; }
    if (!strcmp(action, "start-print")) { start_print(); return; }
    if (!strncmp(action, "step:", 5)) { s.step = atoi(action + 5); render(); return; }
    if (!strncmp(action, "move:", 5)) { movement_action(action[5], atoi(action + 7)); return; }
    if (!strcmp(action, "home-axes")) {
        if (!need_connection()) return;
        if (active()) modal(&sw_scr_modal_homeaxes, home_axes_busy);
        else home_axes_busy();
        return;
    }
    if (!strncmp(action, "extrude:", 8)) {
        if (!need_connection()) return;
        if (s.nozzle_now < 170) { toast("Düse zuerst auf mindestens 170 °C aufheizen."); return; }
        extrude_dir = atoi(action + 8);
        if (active()) modal(&sw_scr_modal_extrude, do_extrude);
        else do_extrude();
        return;
    }
    if (!strncmp(action, "history:", 8)) {
        int h = atoi(action + 8);
        file_index = 0;
        for (int i = 0; i < 3; i++) if (h < s.n_history && !strcmp(FILES[i].name, s.history[h].name)) file_index = i;
        go(R_FILE);
        return;
    }
    if (!strncmp(action, "resolve:", 8) || !strncmp(action, "dismiss:", 8)) {
        bool resolve = action[0] == 'r';
        s.message = 0;
        if (resolve && s.status == ST_ERROR) s.status = ST_PAUSED;
        render();
        toast(resolve ? "Hinweis bestätigt · Druck bleibt pausiert" : "Meldung ausgeblendet");
        return;
    }
    if (!strcmp(action, "reconnect")) { busy(&sw_scr_busy_reconnect, reconnect_done); return; }
    if (!strcmp(action, "setup-connect")) { busy(&sw_scr_busy_wifi, wifi_done); return; }
    if (!strcmp(action, "power-switch")) {
        if (s.power) modal(active() ? &sw_scr_modal_poweroff : &sw_scr_modal_poweroff_idle, power_toggle);
        else power_toggle();
        return;
    }
    if (!strcmp(action, "sleep")) { sleep_now(); return; }
    if (!strcmp(action, "wake")) {
        if (ov_sleep) { sw_screen_free(ov_sleep); ov_sleep = NULL; }
        apply_backlight();
        reset_idle();
        return;
    }
    if (!strcmp(action, "restart")) { modal(&sw_scr_modal_restart, restart_busy); return; }
    if (!strcmp(action, "update")) { busy(&sw_scr_busy_update, update_done); return; }
    if (!strcmp(action, "factory")) { modal(&sw_scr_modal_factory, factory_done); return; }
    if (!strncmp(action, "state:", 6)) {
        for (int i = 0; i < 9; i++) if (!strcmp(action + 6, STATUS_NAMES[i])) { state_change((status_t)i); return; }
    }
}

/* Klicks kommen aus Ereignissen der Objekte, die render() löscht -> erst danach ausführen */
static char pending_action[48];
static void dispatch_async(void *p)
{
    (void)p;
    dispatch(pending_action);
}

static void on_action(const char *action, lv_obj_t *obj)
{
    (void)obj;
    snprintf(pending_action, sizeof(pending_action), "%s", action);
    lv_async_call(dispatch_async, NULL);
}

/* ------------------------------------------------------------------ Simulation (setInterval 2 s im Prototyp) */

static int elapsed;
static void tick(lv_timer_t *t)
{
    (void)t;
    if (!s.connected) return;
    bool changed = false;
    int *now[2] = { &s.nozzle_now, &s.bed_now }, tgt[2] = { s.nozzle, s.bed }, stepmax[2] = { 12, 3 };
    for (int k = 0; k < 2; k++) {
        if (*now[k] != tgt[k]) {
            int d = tgt[k] - *now[k];
            int st = abs(d) < stepmax[k] ? abs(d) : stepmax[k];
            *now[k] += d > 0 ? st : -st;
            changed = true;
        }
    }
    if (s.auto_off && s.status == ST_FINISHED && s.nozzle_now < 50 && s.power) {
        s.power = false;
        s.connected = false;
        toast("Düse abgekühlt · Auto-Aus ausgeführt");
        changed = true;
    }
    if (s.status == ST_HEATING && s.nozzle_now >= s.nozzle && s.bed_now >= s.bed) {
        s.status = s.opt[0] ? ST_LEVELLING : ST_PRINTING;
        elapsed = 0;
        changed = true;
    } else if (s.status == ST_LEVELLING) {
        elapsed += 2;
        if (elapsed >= 8) { s.status = ST_PRINTING; elapsed = 0; changed = true; }
    } else if (s.status == ST_PRINTING) {
        elapsed += 2;
        if (elapsed >= 20) {
            elapsed = 0;
            s.progress = s.progress + 1 > 100 ? 100 : s.progress + 1;
            s.remaining = s.remaining - 2 < 0 ? 0 : s.remaining - 2;
            changed = true;
            if (s.progress >= 100) { finish(); return; }
        }
    }
    if (changed && (route == R_HOME || route == R_TEMPS || route == R_POWER) && !ov_modal) {
        if (pick_def() != scr->def) render();
        else patch();
    }
}

static void any_touch(lv_event_t *e)
{
    (void)e;
    reset_idle();
}

void sw_app_start(lv_obj_t *parent, sw_backlight_cb_t backlight, bool start_asleep)
{
    g_parent = parent;
    g_backlight = backlight;
    defaults();
    sw_render_set_action_cb(on_action);
    sw_render_set_field_cb(on_field);
    if (start_asleep) sleep_now();   /* vor dem ersten Bild, damit das Licht nie kurz angeht */
    render();
    lv_timer_create(tick, 2000, NULL);
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i))
        lv_indev_add_event_cb(i, any_touch, LV_EVENT_PRESSED, NULL);
    if (!start_asleep) reset_idle();
    apply_backlight();
}

void sw_app_force_dark(bool on)
{
    g_force_dark = on;
    apply_backlight();
}

/* Für den Simulator: Zustand direkt setzen */
void sw_app_debug_go(const char *action)
{
    if (!strncmp(action, "press:", 6)) { sw_debug_press(scr, action + 6); return; }
    if (!strcmp(action, "reset")) {   /* Test: Anfangszustand wie nach dem Start (ohne Ruhebildschirm) */
        close_modal();
        if (ov_sleep) { sw_screen_free(ov_sleep); ov_sleep = NULL; }
        if (ov_toast) lv_obj_add_flag(ov_toast->root, LV_OBJ_FLAG_HIDDEN);
        defaults();
        file_source = file_index = slot_index = wizard_step = wifi_index = 0;
        wizard_load = true;
        route = R_HOME;
        render();
        return;
    }
    dispatch(action);
}
