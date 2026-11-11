/**
 * Soft White – baut Bildschirme aus den erzeugten Tabellen (gen/) und zeichnet sie wie der Prototyp.
 */
#pragma once
#include "sw_types.h"

typedef void (*sw_action_cb_t)(const char *action, lv_obj_t *obj);
typedef void (*sw_field_cb_t)(const char *field, int32_t value, const char *text);

typedef struct {
    const sw_screen_def_t *def;
    lv_obj_t *root;
    lv_obj_t **objs;    /* ein LVGL-Objekt je Knoten (gleicher Index) */
} sw_screen_t;

/* Hintergrundfarbe des Geräts (--surface) */
#define SW_SURFACE lv_color_hex(0xeceee4) /* #eeede9, so gewählt, dass RGB565 die nächste Displayfarbe trifft */

void sw_render_set_action_cb(sw_action_cb_t cb);
void sw_render_set_field_cb(sw_field_cb_t cb);
/* Objekt geändert: im Scrollbild neu zeichnen lassen (die sw_set_*-Funktionen tun das selbst) */
void sw_render_touch(lv_obj_t *o);
int32_t sw_range_get(sw_screen_t *s, int idx);
void sw_range_set(sw_screen_t *s, int idx, int32_t v);
sw_screen_t *sw_screen_build(const sw_screen_def_t *def, lv_obj_t *parent);
void sw_screen_free(sw_screen_t *s);
/* Nur einen Teilbaum (Dialog, Toast, Ruhebildschirm) als Ebene über dem Bildschirm aufbauen */
sw_screen_t *sw_overlay_build(const sw_screen_def_t *def, lv_obj_t *parent, int root_idx);
int sw_key(const sw_screen_def_t *def, const char *key);
void sw_debug_press(sw_screen_t *s, const char *action);
int sw_find_text(const sw_screen_def_t *def, const char *text);
int sw_find_text_in(const sw_screen_def_t *def, int container, const char *text);
int sw_find_action(const sw_screen_def_t *def, const char *action);
void sw_set_width_centered(sw_screen_t *s, int idx, int32_t w);

/* Hilfen für die Logik */
void sw_set_text(sw_screen_t *s, int idx, const char *text);
void sw_set_textf(sw_screen_t *s, int idx, const char *fmt, ...);
void sw_set_selected(sw_screen_t *s, int idx, bool sel);
bool sw_get_selected(sw_screen_t *s, int idx);
void sw_set_disabled(sw_screen_t *s, int idx, bool dis);
void sw_set_hidden(sw_screen_t *s, int idx, bool hidden);
void sw_set_icon(sw_screen_t *s, int idx, const lv_image_dsc_t *img);
void sw_set_color(sw_screen_t *s, int idx, lv_color_t c);
void sw_set_box_style(sw_screen_t *s, int idx, const sw_style_t *st);
void sw_set_opa(sw_screen_t *s, int idx, lv_opa_t opa);
void sw_set_height_from_bottom(sw_screen_t *s, int idx, int32_t h);
lv_obj_t *sw_obj(sw_screen_t *s, int idx);
