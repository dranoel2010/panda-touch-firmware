/**
 * Soft White – Datentypen für die aus dem Prototyp erzeugten Bildschirme.
 * Die Tabellen in gen/ werden von tools/design/gen.py erzeugt; bitte nicht von Hand ändern.
 */
#pragma once
#include <stdint.h>
#include "lvgl.h"

/* Alpha-Maske eines einzelnen box-shadow, verlustfrei gekürzt:
 * Spalten [0,l) fest, [l,l+mw) Mittelband (wird gekachelt), Rest fest; Zeilen analog mit t/mh. */
typedef struct {
    const uint8_t *data;
    uint16_t w, h;          /* gespeicherte Größe */
    uint16_t l, mw, t, mh;  /* fester linker/oberer Teil, Breite/Höhe des Mittelbands */
    int16_t ox, oy;         /* Lage der Maske relativ zur Box (außen: -Rand) */
    uint16_t grow_w, grow_h;/* Maskengröße minus Boxgröße */
} sw_mask_t;

typedef struct {
    const sw_mask_t *mask;
    lv_color_t color;
    lv_opa_t opa;
    uint8_t inset;
} sw_shadow_t;

enum { SW_BG_NONE = 0, SW_BG_COLOR, SW_BG_LINEAR };

typedef struct {
    uint8_t bg;                 /* SW_BG_* */
    lv_color_t c1, c2;          /* Farbe bzw. Verlauf von c1 nach c2 */
    lv_opa_t o1, o2;
    int16_t angle;              /* CSS-Winkel des Verlaufs in Grad */
    uint16_t radius;            /* wirksamer Radius in px */
    uint8_t border_w;
    uint8_t border_bottom;      /* nur untere Kante (setting-row) */
    lv_color_t border_c;
    lv_opa_t border_o;
    uint8_t n_shadows;
    const sw_shadow_t *shadows; /* CSS-Reihenfolge: erster liegt oben */
    lv_color_t text_color;      /* geerbte Schriftfarbe dieses Zustands */
    int8_t translate_y;
    lv_opa_t opacity;
} sw_style_t;

enum { SW_N_BOX = 0, SW_N_LABEL, SW_N_ICON, SW_N_IMG, SW_N_DIVIDER, SW_N_SWITCH };
enum { SW_AL_LEFT = 0, SW_AL_CENTER, SW_AL_RIGHT };

typedef struct {
    uint8_t kind;
    uint8_t container;          /* wird als eigenes LVGL-Elternobjekt angelegt */
    uint8_t scroll;             /* scrollbarer Bereich (page-content) */
    uint8_t inherit_color;      /* Text/Icon folgt der Schriftfarbe des Containers */
    int16_t parent;             /* Index des Container-Knotens, -1 = Bildschirm */
    int16_t x, y, w, h;         /* relativ zum Elternobjekt */
    int16_t pad_bottom;         /* nur scroll */
    const sw_style_t *st;       /* Box: normal */
    const sw_style_t *st_pressed;
    const sw_style_t *st_sel;
    const sw_style_t *st_sel_pressed;
    uint8_t selected;           /* Startzustand "selected" */
    uint8_t disabled;
    const char *action;         /* data-action des Prototyps */
    const lv_font_t *font;      /* Label */
    lv_color_t color;           /* Label/Icon */
    lv_opa_t opa;
    int16_t ls_q4;              /* Label: Laufweite in 1/16 px (x = Bezugspunkt in 1/16 px) */
    uint8_t hidden;             /* Start unsichtbar (opacity 0 im Prototyp) */
    uint8_t align;
    const char *text;
    const lv_image_dsc_t *img;  /* Icon (A8) oder Bild */
    const char *key;            /* stabiler Schlüssel aus der Prototyp-Struktur, z. B. "text:remaining/strong" */
} sw_node_t;

typedef struct {
    const char *name;
    const sw_node_t *nodes;
    uint16_t count;
} sw_screen_def_t;
