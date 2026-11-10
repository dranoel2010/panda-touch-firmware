/**
 * Soft White – App-Logik (Portierung von design/soft-white/prototype/app.js).
 * Solange der Drucker noch nicht angebunden ist, laufen dieselben Demo-Daten wie im Prototyp.
 */
#pragma once
#include "lvgl.h"

typedef void (*sw_backlight_cb_t)(int percent);

/* Startet die Oberfläche. start_asleep: mit Ruhebildschirm und ausgeschaltetem Licht beginnen. */
void sw_app_start(lv_obj_t *parent, sw_backlight_cb_t backlight, bool start_asleep);
