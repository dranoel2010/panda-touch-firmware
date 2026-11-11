/**
 * Soft White – Text mit Subpixel-Laufweite wie im Browser.
 *
 * LVGL rundet jede Zeichenbreite auf ganze Pixel und kennt nur ganzzahlige Laufweite (letter-spacing).
 * Der Prototyp nutzt z. B. -1,5 px; das summiert sich sonst über ein Wort zu mehreren Pixeln.
 * Hier wird in 1/16 px gerechnet (Breite + Kerning + Laufweite) und erst pro Zeichen gerundet.
 */
#include "sw_text.h"
#include "sw_render.h"
#include <string.h>

typedef struct {
    char *text;
    const lv_font_t *font;
    int32_t ls_q4;      /* Laufweite in 1/16 px */
    int32_t anchor_q4;  /* Bezugspunkt x in 1/16 px relativ zum Objekt */
    uint8_t align;
} sw_text_t;

/* UTF-8: nächstes Zeichen ab *i, 0 am Ende */
static uint32_t utf8_next(const char *s, uint32_t *i)
{
    const uint8_t *p = (const uint8_t *)s + *i;
    uint32_t c = p[0];
    if (!c) return 0;
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
    if (n == 2) c = ((c & 0x1f) << 6) | (p[1] & 0x3f);
    else if (n == 3) c = ((c & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f);
    else if (n == 4) c = ((c & 0x07) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f);
    *i += n;
    return c;
}

static int8_t kern_value(const lv_font_fmt_txt_dsc_t *f, uint32_t gl, uint32_t gr)
{
    if (!f->kern_dsc || !gl || !gr) return 0;
    if (f->kern_classes) {
        const lv_font_fmt_txt_kern_classes_t *k = f->kern_dsc;
        uint8_t lc = k->left_class_mapping[gl], rc = k->right_class_mapping[gr];
        if (lc && rc) return k->class_pair_values[(lc - 1) * k->right_class_cnt + (rc - 1)];
        return 0;
    }
    const lv_font_fmt_txt_kern_pair_t *k = f->kern_dsc;
    for (uint32_t i = 0; i < k->pair_cnt; i++) {
        uint32_t a, b;
        if (k->glyph_ids_size == 0) {
            a = ((const uint8_t *)k->glyph_ids)[i * 2];
            b = ((const uint8_t *)k->glyph_ids)[i * 2 + 1];
        } else {
            a = ((const uint16_t *)k->glyph_ids)[i * 2];
            b = ((const uint16_t *)k->glyph_ids)[i * 2 + 1];
        }
        if (a == gl && b == gr) return k->values[i];
    }
    return 0;
}

static uint32_t gid_of(const lv_font_t *font, uint32_t cp)
{
    lv_font_glyph_dsc_t g;
    if (!lv_font_get_glyph_dsc(font, &g, cp, 0)) return 0;
    return g.gid.index;
}

/* Vorschub eines Zeichens inkl. Kerning zum nächsten, in 1/16 px */
static int32_t advance_q4(const lv_font_t *font, uint32_t cp, uint32_t next)
{
    const lv_font_fmt_txt_dsc_t *f = font->dsc;
    uint32_t g = gid_of(font, cp);
    if (!g) return 0;
    int32_t adv = f->glyph_dsc[g].adv_w;
    if (next) adv += ((int32_t)kern_value(f, g, gid_of(font, next)) * f->kern_scale) >> 4;
    return adv;
}

int32_t sw_text_width_q4(const lv_font_t *font, const char *txt, int32_t ls_q4)
{
    int32_t w = 0;
    uint32_t i = 0;
    uint32_t cp = utf8_next(txt, &i);
    while (cp) {
        uint32_t ni = i;
        uint32_t next = utf8_next(txt, &ni);
        w += advance_q4(font, cp, next) + ls_q4;   /* Browser: Laufweite nach jedem Zeichen */
        cp = next;
        i = ni;
    }
    return w;
}

static void text_event(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    sw_text_t *t = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        lv_free(t->text);
        lv_free(t);
        return;
    }
    extern int sw_prof_skip;
    if (code != LV_EVENT_DRAW_MAIN || !t->text || !t->text[0] || (sw_prof_skip & 4)) return;

    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t c;
    lv_obj_get_coords(obj, &c);
    int32_t w = sw_text_width_q4(t->font, t->text, t->ls_q4);
    int32_t pen = (c.x1 << 4) + t->anchor_q4;
    if (t->align == SW_AL_CENTER) pen -= w / 2;
    else if (t->align == SW_AL_RIGHT) pen -= w;

    lv_draw_letter_dsc_t d;
    lv_draw_letter_dsc_init(&d);
    d.font = t->font;
    d.color = lv_obj_get_style_text_color(obj, 0);
    d.opa = lv_obj_get_style_text_opa(obj, 0);
    uint32_t i = 0;
    uint32_t cp = utf8_next(t->text, &i);
    while (cp) {
        uint32_t ni = i;
        uint32_t next = utf8_next(t->text, &ni);
        if (cp != ' ') {
            d.unicode = cp;
            /* lv_draw_letter positioniert am Drehpunkt: halbe (gerundete) Vorschubbreite, Grundlinie */
            lv_font_glyph_dsc_t g;
            lv_font_get_glyph_dsc(t->font, &g, cp, 0);
            lv_point_t p = { ((pen + 8) >> 4) + g.adv_w / 2, c.y1 + t->font->line_height - t->font->base_line };
            lv_draw_letter(layer, &d, &p);
        }
        pen += advance_q4(t->font, cp, next) + t->ls_q4;
        cp = next;
        i = ni;
    }
}

lv_obj_t *sw_text_create(lv_obj_t *parent, const lv_font_t *font, int32_t ls_q4, uint8_t align, int32_t anchor_q4,
                         int32_t y, const char *text)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    sw_text_t *t = lv_malloc_zeroed(sizeof(*t));
    t->font = font;
    t->ls_q4 = ls_q4;
    t->align = align;
    t->anchor_q4 = anchor_q4;
    t->text = lv_strdup(text ? text : "");
    /* Objekt über die volle Elternbreite; gezeichnet wird ab dem Bezugspunkt */
    lv_obj_set_pos(o, 0, y);
    lv_obj_set_size(o, LV_PCT(100), lv_font_get_line_height(font));
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(o, text_event, LV_EVENT_ALL, t);
    return o;
}

static sw_text_t *data_of(lv_obj_t *o)
{
    for (uint32_t i = 0; i < lv_obj_get_event_count(o); i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(o, i);
        if (lv_event_dsc_get_cb(d) == text_event) return lv_event_dsc_get_user_data(d);
    }
    return NULL;
}

void sw_text_set(lv_obj_t *o, const char *text)
{
    sw_text_t *t = data_of(o);
    if (!t || !strcmp(t->text, text)) return;
    lv_free(t->text);
    t->text = lv_strdup(text);
    sw_render_touch(o);
    lv_obj_invalidate(o);
}

const char *sw_text_get(lv_obj_t *o)
{
    sw_text_t *t = data_of(o);
    return t ? t->text : "";
}
