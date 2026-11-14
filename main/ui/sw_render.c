/**
 * Soft White – Zeichen-Engine.
 *
 * Jede Box wird in derselben Reihenfolge gezeichnet wie im Browser:
 *   äußere Schatten (Masken, letzter zuerst) -> Hintergrund/Verlauf -> innere Schatten -> Rahmen -> Inhalt.
 * Schatten sind Alpha-Masken aus Chrome, verlustfrei gekürzt; das Mittelband wird gekachelt.
 */
#include "sw_render.h"
#include "sw_text.h"
#include "src/core/lv_refr_private.h"
#include "src/display/lv_display_private.h"
#include "src/draw/lv_draw_private.h"
#include "src/core/lv_obj_draw_private.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#define GRAD_ALLOC(n) heap_caps_malloc((n), MALLOC_CAP_SPIRAM)   /* internes RAM für WLAN freihalten */
#define BIG_FREE(p) heap_caps_free(p)
#else
#define GRAD_ALLOC(n) lv_malloc(n)
#define BIG_FREE(p) lv_free(p)
#endif
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sw_action_cb_t s_action_cb;
int sw_prof_skip;   /* nur zur Messung: 1 = keine Schatten, 2 = keine Flächen, 4 = keine Texte, 8 = kein Scrollbild */

typedef struct {
    sw_screen_t *scr;
    int idx;
    const sw_style_t *override;
} box_rt_t;
static box_rt_t *rt_of(lv_obj_t *o);

/* ------------------------------------------------------------------ Masken-Stücke */

/* Zu jeder Maske 9 dauerhafte Bildbeschreibungen (LVGL zeichnet verzögert, darf nicht auf den Stack zeigen). */
typedef struct piece_cache {
    const sw_mask_t *m;
    lv_image_dsc_t p[9];
    uint8_t empty[9];   /* Stück vollständig transparent -> gar nicht zeichnen */
    lv_area_t trim[9];  /* nicht-transparenter Bereich innerhalb des Stücks */
    lv_image_dsc_t sub[9];
    lv_image_dsc_t big[9];  /* gekachelte Stücke vervielfacht (siehe big_tile) */
    uint8_t big_tried[9];
    struct piece_cache *next;
} piece_cache_t;
static piece_cache_t *s_pieces;

static piece_cache_t *pieces_for(const sw_mask_t *m)
{
    for (piece_cache_t *c = s_pieces; c; c = c->next) {
        if (c->m == m) return c;
    }
    piece_cache_t *c = lv_malloc_zeroed(sizeof(*c));
    c->m = m;
    const uint16_t xs[4] = { 0, m->l, (uint16_t)(m->l + m->mw), m->w };
    const uint16_t ys[4] = { 0, m->t, (uint16_t)(m->t + m->mh), m->h };
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) {
            lv_image_dsc_t *d = &c->p[r * 3 + k];
            d->header.magic = LV_IMAGE_HEADER_MAGIC;
            d->header.cf = LV_COLOR_FORMAT_A8;
            d->header.w = xs[k + 1] - xs[k];
            d->header.h = ys[r + 1] - ys[r];
            d->header.stride = m->w;
            d->data = m->data + ys[r] * m->w + xs[k];
            d->data_size = (uint32_t)m->w * d->header.h;
            /* Begrenzungsrahmen der nicht-transparenten Pixel; leere Ränder werden nicht gezeichnet.
             * Gekachelte Richtungen (mittlere Spalte/Zeile) bleiben vollständig. */
            int32_t bx0 = d->header.w, by0 = d->header.h, bx1 = -1, by1 = -1;
            for (int32_t yy = 0; yy < (int32_t)d->header.h; yy++)
                for (int32_t xx = 0; xx < (int32_t)d->header.w; xx++)
                    if (d->data[yy * m->w + xx]) {
                        if (xx < bx0) bx0 = xx;
                        if (xx > bx1) bx1 = xx;
                        if (yy < by0) by0 = yy;
                        if (yy > by1) by1 = yy;
                    }
            c->empty[r * 3 + k] = bx1 < 0;
            if (bx1 >= 0) {
                if (k == 1) { bx0 = 0; bx1 = d->header.w - 1; }
                if (r == 1) { by0 = 0; by1 = d->header.h - 1; }
                c->trim[r * 3 + k] = (lv_area_t){ bx0, by0, bx1, by1 };
            }
        }
    }
    c->next = s_pieces;
    s_pieces = c;
    return c;
}

static bool area_intersect(lv_area_t *r, const lv_area_t *a, const lv_area_t *b)
{
    r->x1 = LV_MAX(a->x1, b->x1);
    r->y1 = LV_MAX(a->y1, b->y1);
    r->x2 = LV_MIN(a->x2, b->x2);
    r->y2 = LV_MIN(a->y2, b->y2);
    return r->x1 <= r->x2 && r->y1 <= r->y2;
}

/* Ein Maskenstück zeichnen; tx/ty: in diese Richtung kacheln. Eigene Kachelung statt LVGLs "tile",
 * weil diese an Clip-Kanten (z. B. Rand des Scrollbereichs) das Muster versetzt. */
static void draw_piece(lv_layer_t *layer, const lv_draw_image_dsc_t *d, const lv_image_dsc_t *img, const lv_area_t *a,
                       bool tx, bool ty)
{
    int32_t tw = img->header.w, th = img->header.h;
    lv_area_t save = layer->_clip_area, clip;
    if (!area_intersect(&clip, &save, a)) return;
    layer->_clip_area = clip;
    for (int32_t y = a->y1; y <= a->y2; y += th) {
        for (int32_t x = a->x1; x <= a->x2; x += tw) {
            lv_area_t t = { x, y, x + tw - 1, y + th - 1 };
            lv_area_t o;
            if (area_intersect(&o, &t, &clip)) lv_draw_image(layer, d, &t);
            if (!tx) break;
        }
        if (!ty) break;
    }
    layer->_clip_area = save;
}

/* Gekachelte Stücke sind in der Datei nur 1–32 px breit/hoch. So gezeichnet, braucht ein großer Schatten
 * hunderte bis über tausend einzelne Zeichenaufträge – das kostete rund die Hälfte der Zeit eines Bildaufbaus.
 * Deshalb wird das Stück einmal zu einer größeren Kachel vervielfacht (ganze Wiederholungen, also pixelgleich). */
#define BIG_TILE_W 96
#define BIG_TILE_H 64
static const lv_image_dsc_t *big_tile(piece_cache_t *pc, int i, const lv_image_dsc_t *src, bool tx, bool ty)
{
    if (pc->big[i].data) return &pc->big[i];
    if (pc->big_tried[i]) return src;
    pc->big_tried[i] = 1;
    int32_t w = src->header.w, h = src->header.h;
    int32_t nx = tx && w < BIG_TILE_W ? (BIG_TILE_W + w - 1) / w : 1;
    int32_t ny = ty && h < BIG_TILE_H ? (BIG_TILE_H + h - 1) / h : 1;
    if (nx == 1 && ny == 1) return src;
    int32_t W = w * nx, H = h * ny;
    uint8_t *d = GRAD_ALLOC((size_t)W * H);
    if (!d) return src;
    for (int32_t y = 0; y < H; y++) {
        const uint8_t *sr = src->data + (y % h) * src->header.stride;
        uint8_t *dr = d + y * W;
        for (int32_t x = 0; x < nx; x++) memcpy(dr + x * w, sr, w);
    }
    lv_image_dsc_t *b = &pc->big[i];
    *b = *src;
    b->header.w = W;
    b->header.h = H;
    b->header.stride = W;
    b->data = d;
    b->data_size = (uint32_t)W * H;
    return b;
}

/* Maske in Zielgröße W×H ab (x,y) zeichnen */
/* cover: Bereich, den die deckende Fläche der Box gleich danach vollständig übermalt (oder NULL).
 * Ein äußerer Schatten liegt zum größten Teil darunter – dort zu zeichnen wäre verschenkte Zeit. */
static void draw_mask(lv_layer_t *layer, const sw_mask_t *m, int32_t x, int32_t y, int32_t W, int32_t H,
                      lv_color_t color, lv_opa_t opa, const lv_area_t *cover)
{
    if (opa <= LV_OPA_MIN) return;
    piece_cache_t *pc = pieces_for(m);
    lv_image_dsc_t *p = pc->p;
    const int32_t L = m->l, R = m->w - m->l - m->mw, T = m->t, B = m->h - m->t - m->mh;
    int32_t MW = W - L - R, MH = H - T - B;
    if (MW < 0) MW = 0;
    if (MH < 0) MH = 0;
    const int32_t dx[4] = { x, x + L, x + L + MW, x + L + MW + R };
    const int32_t dy[4] = { y, y + T, y + T + MH, y + T + MH + B };
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) {
            lv_area_t a = { dx[k], dy[r], dx[k + 1] - 1, dy[r + 1] - 1 };
            if (a.x2 < a.x1 || a.y2 < a.y1) continue;
            lv_image_dsc_t *img = &p[r * 3 + k];
            if (!img->header.w || !img->header.h || pc->empty[r * 3 + k]) continue;
            /* auf den nicht-transparenten Bereich zuschneiden */
            const lv_area_t *tr = &pc->trim[r * 3 + k];
            if (k != 1) {
                a.x1 += tr->x1;
                a.x2 = a.x1 + (tr->x2 - tr->x1);
            }
            if (r != 1) {
                a.y1 += tr->y1;
                a.y2 = a.y1 + (tr->y2 - tr->y1);
            }
            lv_image_dsc_t *sub = &pc->sub[r * 3 + k];
            if (!sub->data) {
                *sub = *img;
                sub->data = img->data + tr->y1 * img->header.stride + tr->x1;
                sub->header.w = tr->x2 - tr->x1 + 1;
                sub->header.h = tr->y2 - tr->y1 + 1;
                sub->data_size = img->header.stride * sub->header.h;
            }
            img = sub;
            const lv_image_dsc_t *tile = (k == 1 || r == 1) ? big_tile(pc, r * 3 + k, sub, k == 1, r == 1) : sub;
            lv_draw_image_dsc_t d;
            lv_draw_image_dsc_init(&d);
            d.src = tile;
            d.recolor = color;
            d.recolor_opa = LV_OPA_COVER;
            d.opa = opa;
            if (cover) {
                /* nur zeichnen, was neben der deckenden Fläche übrig bleibt (per Beschnitt, die Kacheln
                 * behalten so ihre Lage) */
                lv_area_t in;
                if (area_intersect(&in, &a, cover)) {
                    const lv_area_t rest[4] = {
                        { a.x1, a.y1, a.x2, in.y1 - 1 }, { a.x1, in.y2 + 1, a.x2, a.y2 },
                        { a.x1, in.y1, in.x1 - 1, in.y2 }, { in.x2 + 1, in.y1, a.x2, in.y2 },
                    };
                    lv_area_t save = layer->_clip_area, cl;
                    for (int i = 0; i < 4; i++) {
                        if (rest[i].x2 < rest[i].x1 || rest[i].y2 < rest[i].y1) continue;
                        if (!area_intersect(&cl, &save, &rest[i])) continue;
                        layer->_clip_area = cl;
                        draw_piece(layer, &d, tile, &a, k == 1, r == 1);
                    }
                    layer->_clip_area = save;
                    continue;
                }
            }
            draw_piece(layer, &d, tile, &a, k == 1, r == 1);
        }
    }
}

/* ------------------------------------------------------------------ Box zeichnen */

static const sw_style_t *cur_style(lv_obj_t *obj, box_rt_t *rt)
{
    const sw_node_t *n = &rt->scr->def->nodes[rt->idx];
    if (rt->override) return rt->override;
    bool sel = lv_obj_has_state(obj, LV_STATE_CHECKED);
    bool pr = lv_obj_has_state(obj, LV_STATE_PRESSED) && !lv_obj_has_state(obj, LV_STATE_DISABLED);
    if (sel && n->st_sel) return (pr && n->st_sel_pressed) ? n->st_sel_pressed : n->st_sel;
    return (pr && n->st_pressed) ? n->st_pressed : n->st;
}

/* CSS linear-gradient(angle): Linie durch die Mitte, Länge |w·sin|+|h·cos| */
static void grad_points(int32_t w, int32_t h, int angle, lv_point_t *s, lv_point_t *e)
{
    double a = angle * M_PI / 180.0;
    double dx = sin(a), dy = -cos(a);
    double len = fabs(w * sin(a)) + fabs(h * cos(a));
    double cx = w / 2.0, cy = h / 2.0;
    s->x = (int32_t)lround(cx - dx * len / 2);
    s->y = (int32_t)lround(cy - dy * len / 2);
    e->x = (int32_t)lround(cx + dx * len / 2);
    e->y = (int32_t)lround(cy + dy * len / 2);
}

static void draw_shadows(lv_layer_t *layer, const sw_style_t *st, const lv_area_t *c, bool inset)
{
    if (sw_prof_skip & 1) return;
    /* Deckt die Fläche der Box? Dann übermalt sie alles innerhalb der Box abseits der runden Ecken:
     * den Streifen zwischen den Eckradien in voller Höhe. */
    lv_area_t cover = { c->x1 + st->radius, c->y1, c->x2 - st->radius, c->y2 };
    bool deckt = !inset && !(sw_prof_skip & 2) && cover.x1 <= cover.x2 &&
                 ((st->bg == SW_BG_COLOR && st->o1 == 255) || (st->bg == SW_BG_LINEAR && st->o1 == 255 && st->o2 == 255));
    for (int i = st->n_shadows - 1; i >= 0; i--) {   /* CSS: erster Schatten liegt oben */
        const sw_shadow_t *sh = &st->shadows[i];
        if (sh->inset != inset) continue;
        int32_t w = lv_area_get_width(c), h = lv_area_get_height(c);
        draw_mask(layer, sh->mask, c->x1 + sh->mask->ox, c->y1 + sh->mask->oy,
                  w + sh->mask->grow_w, h + sh->mask->grow_h, sh->color, sh->opa, deckt ? &cover : NULL);
    }
}

/* ------------------------------------------------------------------ Verläufe mit Dithering
 * Das Display hat nur RGB565. Ohne Dithering bekommen die feinen Verläufe des Designs Stufen und
 * Farbstiche (R, G und B springen an verschiedenen Stellen). Daher: CSS-Verlauf in voller Genauigkeit
 * pro Pixel berechnen und mit geordnetem Dithering (Bayer 4×4) auf RGB565 bringen. Zwischenspeicher im PSRAM. */
typedef struct grad_img {
    lv_color_t c1, c2;
    int16_t angle, w, h;
    lv_image_dsc_t img;
    uint32_t used;
    struct grad_img *next;
} grad_img_t;
static grad_img_t *s_grads;
static uint32_t s_grad_bytes, s_grad_clock;
#define GRAD_CACHE_MAX (3u * 1024u * 1024u)

static const uint8_t BAYER4[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };

static const lv_image_dsc_t *grad_image(const sw_style_t *st, int32_t w, int32_t h)
{
    s_grad_clock++;
    for (grad_img_t *g = s_grads; g; g = g->next) {
        if (g->w == w && g->h == h && g->angle == st->angle && lv_color_eq(g->c1, st->c1) && lv_color_eq(g->c2, st->c2)) {
            g->used = s_grad_clock;
            return &g->img;
        }
    }
    uint32_t bytes = (uint32_t)w * h * 2;
    while (s_grad_bytes + bytes > GRAD_CACHE_MAX && s_grads) {   /* ältesten entfernen */
        grad_img_t **pp = &s_grads, **old = &s_grads;
        for (; *pp; pp = &(*pp)->next) if ((*pp)->used < (*old)->used) old = pp;
        grad_img_t *o = *old;
        *old = o->next;
        s_grad_bytes -= (uint32_t)o->w * o->h * 2;
#ifdef ESP_PLATFORM
        heap_caps_free((void *)o->img.data);
#else
        lv_free((void *)o->img.data);
#endif
        lv_free(o);
    }
    uint16_t *px = GRAD_ALLOC(bytes);
    grad_img_t *g = lv_malloc_zeroed(sizeof(*g));
    if (!px || !g) {
#ifdef ESP_PLATFORM
        heap_caps_free(px);
#else
        lv_free(px);
#endif
        lv_free(g);
        return NULL;
    }
    /* Ganzzahlig (Festkomma 16.16): double ist auf dem ESP32-S3 nur in Software vorhanden und sehr langsam.
     * t(x,y) ist linear: t = t0 + x*tx + y*ty, Farbe = c1 + (c2-c1)*t, dann Bayer-Dithering auf RGB565. */
    float a = st->angle * (float)M_PI / 180.0f, dxf = sinf(a), dyf = -cosf(a);
    float len = fabsf(w * sinf(a)) + fabsf(h * cosf(a));
    int32_t tx = (int32_t)(dxf / len * 65536.0f), ty = (int32_t)(dyf / len * 65536.0f);
    int32_t t00 = (int32_t)(((0.5f - w / 2.0f) * dxf + (0.5f - h / 2.0f) * dyf) / len * 65536.0f) + 32768;
    /* Kanäle in Displayeinheiten ×16 (für das 4×4-Dithering): r,b in 0..31*16, g in 0..63*16 */
    int32_t r1 = st->c1.red * 31 * 16 / 255, g1 = st->c1.green * 63 * 16 / 255, b1 = st->c1.blue * 31 * 16 / 255;
    int32_t r2 = st->c2.red * 31 * 16 / 255, g2 = st->c2.green * 63 * 16 / 255, b2 = st->c2.blue * 31 * 16 / 255;
    for (int32_t y = 0; y < h; y++) {
        int32_t t = t00 + y * ty;
        const uint8_t *bay = BAYER4[y & 3];
        uint16_t *row = px + y * w;
        for (int32_t x = 0; x < w; x++, t += tx) {
            int32_t tc = t < 0 ? 0 : (t > 65536 ? 65536 : t);
            int32_t d = bay[x & 3];
            int32_t r = (r1 + (int32_t)(((int64_t)(r2 - r1) * tc) >> 16) + d) >> 4;
            int32_t gg = (g1 + (int32_t)(((int64_t)(g2 - g1) * tc) >> 16) + d) >> 4;
            int32_t b = (b1 + (int32_t)(((int64_t)(b2 - b1) * tc) >> 16) + d) >> 4;
            row[x] = (uint16_t)((LV_MIN(r, 31) << 11) | (LV_MIN(gg, 63) << 5) | LV_MIN(b, 31));
        }
    }
    g->c1 = st->c1;
    g->c2 = st->c2;
    g->angle = st->angle;
    g->w = w;
    g->h = h;
    g->used = s_grad_clock;
    g->img.header.magic = LV_IMAGE_HEADER_MAGIC;
    g->img.header.cf = LV_COLOR_FORMAT_RGB565;
    g->img.header.w = w;
    g->img.header.h = h;
    g->img.header.stride = w * 2;
    g->img.data = (const uint8_t *)px;
    g->img.data_size = bytes;
    g->next = s_grads;
    s_grads = g;
    s_grad_bytes += bytes;
    return &g->img;
}

static void draw_body(lv_layer_t *layer, const sw_style_t *st, const lv_area_t *c)
{
    if (st->bg == SW_BG_NONE || (sw_prof_skip & 2)) return;
    if (st->bg == SW_BG_LINEAR && st->o1 == 255 && st->o2 == 255) {
        const lv_image_dsc_t *img = grad_image(st, lv_area_get_width(c), lv_area_get_height(c));
        if (img) {
            lv_draw_image_dsc_t d;
            lv_draw_image_dsc_init(&d);
            d.src = img;
            d.clip_radius = st->radius;
            lv_draw_image(layer, &d, c);
            return;
        }
    }
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.radius = st->radius;
    d.border_width = 0;
    d.shadow_width = 0;
    d.outline_width = 0;
    d.bg_image_src = NULL;
    if (st->bg == SW_BG_COLOR) {
        d.bg_color = st->c1;
        d.bg_opa = st->o1;
    } else {
        d.bg_opa = LV_OPA_COVER;
        d.bg_grad.stops_count = 2;
        d.bg_grad.stops[0].color = st->c1;
        d.bg_grad.stops[0].opa = st->o1;
        d.bg_grad.stops[0].frac = 0;
        d.bg_grad.stops[1].color = st->c2;
        d.bg_grad.stops[1].opa = st->o2;
        d.bg_grad.stops[1].frac = 255;
        lv_point_t s, e;
        grad_points(lv_area_get_width(c), lv_area_get_height(c), st->angle, &s, &e);
        lv_grad_linear_init(&d.bg_grad, s.x, s.y, e.x, e.y, LV_GRAD_EXTEND_PAD);
        d.bg_color = st->c1;
    }
    lv_draw_rect(layer, &d, c);
}

static void draw_border(lv_layer_t *layer, const sw_style_t *st, const lv_area_t *c)
{
    if (!st->border_w) return;
    lv_draw_border_dsc_t d;
    lv_draw_border_dsc_init(&d);
    d.radius = st->radius;
    d.width = st->border_w;
    d.color = st->border_c;
    d.opa = st->border_o;
    d.side = st->border_bottom ? LV_BORDER_SIDE_BOTTOM : LV_BORDER_SIDE_FULL;
    lv_draw_border(layer, &d, c);
}

/* Schalter-Knopf (.switch::after): 40×40, Verlauf 140°, Schatten 3px 3px 5px #7e847a70 */
extern const sw_mask_t *sw_switch_knob_mask(void);

static void draw_switch_knob(lv_layer_t *layer, lv_obj_t *obj, const lv_area_t *c)
{
    bool on = lv_obj_has_state(obj, LV_STATE_CHECKED);
    lv_area_t k = { c->x1 + 10 + (on ? 34 : 0), c->y1 + 10, 0, 0 };
    k.x2 = k.x1 + 39;
    k.y2 = k.y1 + 39;
    const sw_mask_t *m = sw_switch_knob_mask();
    if (m) draw_mask(layer, m, k.x1 + m->ox, k.y1 + m->oy, 40 + m->grow_w, 40 + m->grow_h, lv_color_hex(0x78847a), 0x70, NULL);
    static const sw_style_t knob = { SW_BG_LINEAR, LV_COLOR_MAKE(0xfa, 0xfa, 0xf7), LV_COLOR_MAKE(0xe8, 0xe8, 0xe4),
                                     255, 255, 140, 20, 0, 0, LV_COLOR_MAKE(0, 0, 0), 0, 0, NULL,
                                     LV_COLOR_MAKE(0, 0, 0), 0, 255 };
    draw_body(layer, &knob, &k);
}

static void apply_variant(lv_obj_t *obj, box_rt_t *rt);

static void box_event(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    box_rt_t *rt = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    const sw_node_t *n = &rt->scr->def->nodes[rt->idx];

    if (code == LV_EVENT_DELETE) {
        lv_free(rt);
        return;
    }
    if (code == LV_EVENT_REFR_EXT_DRAW_SIZE) {
        int32_t ext = 0;
        const sw_style_t *all[4] = { n->st, n->st_pressed, n->st_sel, n->st_sel_pressed };
        for (int i = 0; i < 4; i++) {
            if (!all[i]) continue;
            for (int k = 0; k < all[i]->n_shadows; k++) {
                const sw_mask_t *m = all[i]->shadows[k].mask;
                if (-m->ox > ext) ext = -m->ox;
            }
        }
        if (n->kind == SW_N_SWITCH) ext = LV_MAX(ext, 16);
        lv_event_set_ext_draw_size(e, ext + 2);
        return;
    }
    if (code == LV_EVENT_DRAW_MAIN_BEGIN || code == LV_EVENT_DRAW_MAIN || code == LV_EVENT_DRAW_MAIN_END) {
        const sw_style_t *st = cur_style(obj, rt);
        if (!st) return;
        lv_layer_t *layer = lv_event_get_layer(e);
        lv_area_t c;
        lv_obj_get_coords(obj, &c);
        if (code == LV_EVENT_DRAW_MAIN_BEGIN) draw_shadows(layer, st, &c, false);
        else if (code == LV_EVENT_DRAW_MAIN) draw_body(layer, st, &c);
        else {
            draw_shadows(layer, st, &c, true);
            draw_border(layer, st, &c);
            if (n->kind == SW_N_SWITCH) draw_switch_knob(layer, obj, &c);
        }
        return;
    }
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        apply_variant(obj, rt);
        return;
    }
    if (code == LV_EVENT_CLICKED && n->action && s_action_cb && !lv_obj_has_state(obj, LV_STATE_DISABLED)) {
        s_action_cb(n->action, obj);
    }
}

/* Zustand (normal/gedrückt/ausgewählt) übernehmen: Deckkraft, Versatz, geerbte Schriftfarbe */
static void apply_variant(lv_obj_t *obj, box_rt_t *rt)
{
    const sw_style_t *st = cur_style(obj, rt);
    sw_screen_t *s = rt->scr;
    if (st) {
        lv_obj_set_style_opa_layered(obj, st->opacity, 0);
        sw_render_touch(obj);
        lv_obj_set_style_translate_y(obj, st->translate_y, 0);
        for (uint16_t i = 0; i < s->def->count; i++) {
            const sw_node_t *c = &s->def->nodes[i];
            if (c->parent != rt->idx || !c->inherit_color || !s->objs[i]) continue;
            if (c->kind == SW_N_LABEL) lv_obj_set_style_text_color(s->objs[i], st->text_color, 0);
            else if (c->kind == SW_N_ICON) lv_obj_set_style_image_recolor(s->objs[i], st->text_color, 0);
        }
    }
    sw_render_touch(obj);
    lv_obj_invalidate(obj);
}

/* ------------------------------------------------------------------ Schieberegler (input type=range) */

static sw_field_cb_t s_field_cb;
void sw_render_set_field_cb(sw_field_cb_t cb) { s_field_cb = cb; }

typedef struct {
    const sw_node_t *n;
    int32_t value;
    lv_image_dsc_t left, right;   /* Teilbilder (dauerhaft, da LVGL verzögert zeichnet) */
} range_rt_t;

static int32_t thumb_cx(const range_rt_t *r, int32_t w)
{
    const sw_node_t *n = r->n;
    return 8 + (int32_t)lround((double)(w - 16) * (r->value - n->vmin) / (n->vmax - n->vmin));
}

static void range_event(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_current_target_obj(e);
    range_rt_t *r = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    const sw_node_t *n = r->n;
    if (code == LV_EVENT_DELETE) { lv_free(r); return; }
    if (code == LV_EVENT_DRAW_MAIN) {
        lv_layer_t *layer = lv_event_get_layer(e);
        lv_area_t c;
        lv_obj_get_coords(obj, &c);
        int32_t w = lv_area_get_width(c.x1 <= c.x2 ? &c : &c);
        int32_t cx = thumb_cx(r, w);
        const lv_image_dsc_t *mx = n->img2, *mn = n->img;
        /* voll: links bis zur Knopfmitte */
        r->left = *mx;
        r->left.header.w = LV_MIN(cx, (int32_t)mx->header.w);
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = &r->left;
        lv_area_t a = { c.x1, c.y1 + n->img_y, c.x1 + r->left.header.w - 1, c.y1 + n->img_y + mx->header.h - 1 };
        if (r->left.header.w > 0) lv_draw_image(layer, &d, &a);
        /* leer: ab der Knopfmitte */
        r->right = *mn;
        r->right.header.w = mn->header.w - cx;
        r->right.data = mn->data + cx * 4;
        lv_draw_image_dsc_init(&d);
        d.src = &r->right;
        lv_area_t b = { c.x1 + cx, a.y1, c.x1 + cx + r->right.header.w - 1, a.y1 + mn->header.h - 1 };
        if (r->right.header.w > 0) lv_draw_image(layer, &d, &b);
        /* Knopf */
        lv_draw_image_dsc_init(&d);
        d.src = n->img3;
        lv_area_t k = { c.x1 + cx - 9, a.y1, c.x1 + cx - 9 + n->img3->header.w - 1, a.y1 + n->img3->header.h - 1 };
        lv_draw_image(layer, &d, &k);
        return;
    }
    if (code == LV_EVENT_PRESSING || code == LV_EVENT_PRESSED) {
        lv_point_t p;
        lv_indev_get_point(lv_indev_active(), &p);
        lv_area_t c;
        lv_obj_get_coords(obj, &c);
        int32_t w = lv_area_get_width(&c);
        double f = (double)(p.x - c.x1 - 8) / (w - 16);
        if (f < 0) f = 0;
        if (f > 1) f = 1;
        int32_t step = n->vstep > 0 ? n->vstep : 1;
        int32_t v = n->vmin + (int32_t)lround(f * (n->vmax - n->vmin) / step) * step;
        if (v != r->value) {
            r->value = v;
            sw_render_touch(obj);
            lv_obj_invalidate(obj);
            if (s_field_cb && n->field) s_field_cb(n->field, v, NULL);
        }
    }
}

int32_t sw_range_get(sw_screen_t *s, int idx)
{
    lv_obj_t *o = sw_obj(s, idx);
    if (!o) return 0;
    for (uint32_t i = 0; i < lv_obj_get_event_count(o); i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(o, i);
        if (lv_event_dsc_get_cb(d) == range_event) return ((range_rt_t *)lv_event_dsc_get_user_data(d))->value;
    }
    return 0;
}

void sw_range_set(sw_screen_t *s, int idx, int32_t v)
{
    lv_obj_t *o = sw_obj(s, idx);
    if (!o) return;
    for (uint32_t i = 0; i < lv_obj_get_event_count(o); i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(o, i);
        if (lv_event_dsc_get_cb(d) == range_event) {
            ((range_rt_t *)lv_event_dsc_get_user_data(d))->value = v;
            sw_render_touch(o);
            lv_obj_invalidate(o);
        }
    }
}


/* ------------------------------------------------------------------ Scrollbild
 * Ein scrollbarer Bereich wird einmal vollständig (ganze Inhaltshöhe) in ein RGB565-Bild im PSRAM gezeichnet,
 * in Häppchen im Leerlauf. Während gescrollt wird, zeichnet der Bereich nur noch den passenden Ausschnitt
 * dieses Bildes (reines Kopieren) statt aller Schatten, Verläufe und Texte. Ändert sich etwas im Inhalt
 * (Text, Zustand, Regler …), werden die betroffenen Zeilen im Bild neu gezeichnet. */
typedef struct scache {
    lv_obj_t *cont;
    uint16_t *px;
    int32_t w, h;        /* Bildgröße = Breite des Bereichs × Inhaltshöhe */
    int32_t done;        /* Zeilen 0..done-1 sind gezeichnet */
    int32_t d0, d1;      /* veraltete Zeilen [d0, d1) (Inhaltskoordinaten) */
    bool active;         /* es wird gescrollt: Bild statt Inhalt zeichnen */
    bool probed;
    int32_t nt, nb;      /* Randzeilen oben/unten, in die Darunterliegendes hineinragt */
    uint16_t *under;     /* deren Pixel ohne Inhalt (nt + nb Zeilen) */
    uint16_t *comp;      /* je Bild zusammengesetzt: Inhalt, wo er deckt, sonst Darunterliegendes */
    lv_image_dsc_t img_t, img_b;
    uint32_t born;
    lv_image_dsc_t img;
    struct scache *next;
} scache_t;
static scache_t *s_scaches;
static lv_timer_t *s_sc_timer;
#define SC_CHUNK 48      /* Zeilen je Leerlauf-Häppchen */

static int32_t sc_content_y0(lv_obj_t *c)
{
    lv_area_t a;
    lv_obj_get_coords(c, &a);
    return a.y1 - lv_obj_get_scroll_top(c);
}
static int32_t sc_content_h(lv_obj_t *c)
{
    return lv_obj_get_scroll_top(c) + lv_obj_get_height(c) + LV_MAX(0, lv_obj_get_scroll_bottom(c));
}

/* Objekte in einen RGB565-Puffer zeichnen (wie lv_snapshot, aber ohne Beschnitt durch Eltern).
 * area: Bildschirmbereich, der dem Puffer entspricht; objs: n Objekte in Zeichenreihenfolge. */
static void render_into(uint16_t *px, int32_t w, const lv_area_t *area, lv_obj_t *const *objs, uint32_t n)
{
    int32_t h = lv_area_get_height(area);
    uint16_t bg = lv_color_to_u16(SW_SURFACE);
    for (int32_t i = 0; i < w * h; i++) px[i] = bg;
    if (!n) return;
    lv_draw_buf_t buf;
    lv_draw_buf_init(&buf, w, h, LV_COLOR_FORMAT_RGB565, w * 2, px, (uint32_t)w * h * 2);
    lv_layer_t layer;
    lv_layer_init(&layer);
    lv_display_t *disp = lv_obj_get_display(objs[0]);
    layer.draw_buf = &buf;
    layer.display = disp;
    layer.color_format = LV_COLOR_FORMAT_RGB565;
    layer.buf_area = *area;
    layer._clip_area = *area;
    layer.phy_clip_area = *area;
    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_CREATED, &layer);
    lv_display_t *disp_old = lv_refr_get_disp_refreshing();
    lv_layer_t *head_old = disp->layer_head;
    disp->layer_head = &layer;
    lv_refr_set_disp_refreshing(disp);
    for (uint32_t i = 0; i < n; i++) lv_obj_refr(&layer, objs[i]);
    layer.all_tasks_added = true;
    while (layer.draw_task_head) {
#if LV_USE_OS
        lv_draw_dispatch_wait_for_request();
#endif
        lv_draw_dispatch();
    }
    disp->layer_head = head_old;
    lv_refr_set_disp_refreshing(disp_old);
    lv_draw_unit_send_event(NULL, LV_EVENT_CHILD_DELETED, &layer);
}

/* Zeilen [r0, r1) des Inhalts ins Bild zeichnen */
static void sc_render(scache_t *sc, int32_t r0, int32_t r1)
{
    lv_obj_t *c = sc->cont;
    if (r1 <= r0) return;
    lv_area_t ca;
    lv_obj_get_coords(c, &ca);
    int32_t y0 = sc_content_y0(c);
    lv_area_t area = { ca.x1, y0 + r0, ca.x1 + sc->w - 1, y0 + r1 - 1 };
    uint32_t n = lv_obj_get_child_count(c);
    lv_obj_t **objs = lv_malloc(sizeof(lv_obj_t *) * (n ? n : 1));
    for (uint32_t i = 0; i < n; i++) objs[i] = lv_obj_get_child(c, i);
    if (n) render_into(sc->px + r0 * sc->w, sc->w, &area, objs, n);
    else {
        uint16_t bg = lv_color_to_u16(SW_SURFACE);
        for (int32_t i = r0 * sc->w; i < r1 * sc->w; i++) sc->px[i] = bg;
    }
    lv_free(objs);
}

/* Was vor dem Bereich (darunter) gezeichnet wird und hineinragt (z. B. Unterlängen oder Schatten der
 * Überschrift), darf das undurchsichtige Bild nicht einfach überdecken. Einmal pixelgenau ermitteln, welche
 * Randzeilen betroffen sind, und deren Pixel merken; beim Scrollen wird dort Inhalt über Darunterliegendes
 * gelegt (wo der Inhalt nur Hintergrund ist, scheint das Darunterliegende durch). */
static void sc_probe(scache_t *sc)
{
    lv_obj_t *c = sc->cont, *par = lv_obj_get_parent(c);
    lv_area_t ca;
    lv_obj_get_coords(c, &ca);
    int32_t w = lv_area_get_width(&ca), h = lv_area_get_height(&ca);
    BIG_FREE(sc->under);
    BIG_FREE(sc->comp);
    sc->under = sc->comp = NULL;
    sc->nt = sc->nb = 0;
    sc->probed = true;
    uint32_t n = 0;
    lv_obj_t *objs[32];
    for (uint32_t i = 0; par && i < lv_obj_get_child_count(par) && n < 32; i++) {
        lv_obj_t *sib = lv_obj_get_child(par, i);
        if (sib == c) break;
        lv_area_t sa, o;
        lv_obj_get_coords(sib, &sa);
        lv_area_increase(&sa, lv_obj_get_ext_draw_size(sib), lv_obj_get_ext_draw_size(sib));
        if (!lv_obj_has_flag(sib, LV_OBJ_FLAG_HIDDEN) && area_intersect(&o, &sa, &ca)) objs[n++] = sib;
    }
    if (!n) return;
    uint16_t *px = GRAD_ALLOC((size_t)w * h * 2);
    if (!px) { sc->probed = false; return; }
    render_into(px, w, &ca, objs, n);
    uint16_t bg = lv_color_to_u16(SW_SURFACE);
    int32_t top = -1, bot = h;
    for (int32_t r = 0; r < h; r++) {
        bool any = false;
        for (int32_t x = 0; x < w && !any; x++) any = px[r * w + x] != bg;
        if (!any) continue;
        if (r < h / 2) top = r;
        else if (bot == h) bot = r;
    }
    sc->nt = top + 1;
    sc->nb = h - bot;
    if (sc->nt + sc->nb) {
        size_t sz = (size_t)w * (sc->nt + sc->nb) * 2;
        sc->under = GRAD_ALLOC(sz);
        sc->comp = GRAD_ALLOC(sz);
        if (!sc->under || !sc->comp) { sc->probed = false; BIG_FREE(px); return; }
        memcpy(sc->under, px, (size_t)w * sc->nt * 2);
        memcpy(sc->under + w * sc->nt, px + w * bot, (size_t)w * sc->nb * 2);
    }
#ifndef ESP_PLATFORM
    if (getenv("SW_BANDDBG")) fprintf(stderr, "rand oben %d unten %d\n", (int)sc->nt, (int)sc->nb);
#endif
    BIG_FREE(px);
}

/* Bild auf Stand bringen; budget = höchstens so viele Zeilen (<=0: alles) */
static void sc_sync(scache_t *sc, int32_t budget)
{
    lv_obj_t *c = sc->cont;
    lv_obj_update_layout(c);
    int32_t w = lv_obj_get_width(c), h = sc_content_h(c);
    if (!sc->px || w != sc->w || h != sc->h) {
        BIG_FREE(sc->px);
        sc->px = GRAD_ALLOC((size_t)w * h * 2);
        sc->w = w;
        sc->h = h;
        sc->done = 0;
        sc->d0 = sc->d1 = 0;
        if (!sc->px) return;
    }
    if (budget <= 0) budget = INT32_MAX;
    if (!sc->probed) {
        sc_probe(sc);
        if (budget != INT32_MAX) return;
    }
    if (sc->done < sc->h) {
        int32_t n = LV_MIN(budget, sc->h - sc->done);
        sc_render(sc, sc->done, sc->done + n);
        sc->done += n;
        return;
    }
    if (sc->d1 > sc->d0) {
        int32_t a = LV_MAX(0, sc->d0), b = LV_MIN(sc->h, sc->d1);
        sc->d0 = sc->d1 = 0;
        sc_render(sc, a, b);
    }
}

static bool sc_ready(scache_t *sc)
{
    if (sw_prof_skip & 8) return false;   /* Messung/Vergleich: Scrollbild aus */
    return sc->px && sc->probed && sc->done >= sc->h && sc->d1 <= sc->d0 && sc->w == lv_obj_get_width(sc->cont)
           && sc->h == sc_content_h(sc->cont);
}

static bool sc_pressed(void)
{
    for (lv_indev_t *i = lv_indev_get_next(NULL); i; i = lv_indev_get_next(i))
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER && lv_indev_get_state(i) == LV_INDEV_STATE_PRESSED) return true;
    return false;
}

static void sc_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (sc_pressed()) return;   /* Berührung hat Vorrang */
    for (scache_t *sc = s_scaches; sc; sc = sc->next) {
        if (lv_tick_elaps(sc->born) < 400) continue;   /* erst den neuen Bildschirm zeigen */
        if (!sc_ready(sc)) {
            sc_sync(sc, SC_CHUNK);
            return;   /* ein Häppchen je Durchlauf */
        }
    }
}

/* Ein Objekt hat sich geändert: seine Zeilen im Scrollbild als veraltet markieren */
void sw_render_touch(lv_obj_t *o)
{
    if (!s_scaches || !o) return;
    for (lv_obj_t *p = lv_obj_get_parent(o); p; p = lv_obj_get_parent(p)) {
        for (scache_t *sc = s_scaches; sc; sc = sc->next) {
            if (sc->cont != p) continue;
            lv_area_t a;
            lv_obj_get_coords(o, &a);
            int32_t ext = lv_obj_get_ext_draw_size(o) + 8;
            int32_t y0 = sc_content_y0(p);
            int32_t r0 = a.y1 - ext - y0, r1 = a.y2 + ext + 1 - y0;
            if (sc->d1 <= sc->d0) { sc->d0 = r0; sc->d1 = r1; }
            else { sc->d0 = LV_MIN(sc->d0, r0); sc->d1 = LV_MAX(sc->d1, r1); }
            return;
        }
    }
}

/* Hintergrund (--surface) des Bildschirms; ausgespart, wo gerade ein Scrollbild deckend darüber kommt */
#ifdef ESP_PLATFORM
#include "esp_timer.h"
int64_t sw_t_root, sw_t_sc0, sw_t_sc1;
#define TMARK(v) (v) = esp_timer_get_time()
#else
#define TMARK(v)
#endif
static void root_event(lv_event_t *e)
{
    TMARK(sw_t_root);
    if (sw_prof_skip & 32) return;
    lv_obj_t *root = lv_event_get_current_target_obj(e);
    lv_layer_t *layer = lv_event_get_layer(e);
    lv_area_t all;
    lv_obj_get_coords(root, &all);
    lv_area_t hole = { 0, 0, -1, -1 };
    for (scache_t *sc = s_scaches; sc; sc = sc->next) {
        if (lv_obj_get_parent(sc->cont) != root || !sc->active || !sc_ready(sc)) continue;
        lv_obj_get_coords(sc->cont, &hole);
        break;
    }
    lv_draw_rect_dsc_t r;
    lv_draw_rect_dsc_init(&r);
    r.bg_color = SW_SURFACE;
    r.bg_opa = LV_OPA_COVER;
    if (hole.x2 < hole.x1 || hole.y2 < hole.y1) {
        lv_draw_rect(layer, &r, &all);
        return;
    }
    lv_area_t parts[4] = {
        { all.x1, all.y1, all.x2, hole.y1 - 1 },
        { all.x1, hole.y2 + 1, all.x2, all.y2 },
        { all.x1, hole.y1, hole.x1 - 1, hole.y2 },
        { hole.x2 + 1, hole.y1, all.x2, hole.y2 },
    };
    for (int i = 0; i < 4; i++)
        if (parts[i].x1 <= parts[i].x2 && parts[i].y1 <= parts[i].y2) lv_draw_rect(layer, &r, &parts[i]);
}

static void scache_event(lv_event_t *e)
{
    scache_t *sc = lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        for (scache_t **pp = &s_scaches; *pp; pp = &(*pp)->next)
            if (*pp == sc) { *pp = sc->next; break; }
        BIG_FREE(sc->px);
        BIG_FREE(sc->under);
        BIG_FREE(sc->comp);
        lv_free(sc);
        return;
    }
    if (code == LV_EVENT_SCROLL_BEGIN) {
        sc_sync(sc, 0);   /* fehlende/veraltete Zeilen jetzt sofort */
        if (sc->d1 > sc->d0 || sc->done < sc->h) sc_sync(sc, 0);
        sc->active = true;
        return;
    }
    if (code == LV_EVENT_SCROLL) {
        if (sc->active && !sc_ready(sc)) { sc_sync(sc, 0); sc_sync(sc, 0); }
        return;
    }
    if (code == LV_EVENT_SCROLL_END) {
        sc->active = false;
        /* Randzeilen sind beim Scrollen nur angenähert (halbtransparenter Inhalt über Darunterliegendem):
         * im Stillstand exakt nachzeichnen */
        lv_area_t ca;
        lv_obj_get_coords(sc->cont, &ca);
        if (sc->nt) { lv_area_t t = { ca.x1, ca.y1, ca.x2, ca.y1 + sc->nt - 1 }; lv_obj_invalidate_area(sc->cont, &t); }
        if (sc->nb) { lv_area_t b = { ca.x1, ca.y2 - sc->nb + 1, ca.x2, ca.y2 }; lv_obj_invalidate_area(sc->cont, &b); }
        return;
    }
    if (code == LV_EVENT_DRAW_MAIN_END) {
        TMARK(sw_t_sc0);
        if (!sc->active || !sc_ready(sc)) return;
        lv_layer_t *layer = lv_event_get_layer(e);
        lv_obj_t *c = sc->cont;
        lv_area_t ca;
        lv_obj_get_coords(c, &ca);
        const int32_t w = sc->w, y0 = sc_content_y0(c);
        lv_area_t save = layer->_clip_area, clip;
        /* Mitte: Ausschnitt des Scrollbilds */
        sc->img.header.magic = LV_IMAGE_HEADER_MAGIC;
        sc->img.header.cf = LV_COLOR_FORMAT_RGB565;
        sc->img.header.w = w;
        sc->img.header.h = sc->h;
        sc->img.header.stride = w * 2;
        sc->img.data = (const uint8_t *)sc->px;
        sc->img.data_size = (uint32_t)w * sc->h * 2;
        lv_draw_image_dsc_t d;
        lv_draw_image_dsc_init(&d);
        d.src = &sc->img;
        lv_area_t a = { ca.x1, y0, ca.x1 + w - 1, y0 + sc->h - 1 };
        lv_area_t mid = { ca.x1, ca.y1 + sc->nt, ca.x2, ca.y2 - sc->nb };
        if (area_intersect(&clip, &save, &mid)) {
            layer->_clip_area = clip;
            if (!(sw_prof_skip & 16)) lv_draw_image(layer, &d, &a);
        }
        if (sw_prof_skip & 64) { layer->_clip_area = save; layer->opa = LV_OPA_TRANSP; TMARK(sw_t_sc1); return; }
        /* Ränder: Inhalt über Darunterliegendem */
        const uint16_t bg = lv_color_to_u16(SW_SURFACE);
        for (int band = 0; band < 2; band++) {
            int32_t rows = band ? sc->nb : sc->nt;
            if (!rows) continue;
            int32_t sy = band ? ca.y2 - rows + 1 : ca.y1;   /* erste Bildschirmzeile */
            lv_area_t ba = { ca.x1, sy, ca.x1 + w - 1, sy + rows - 1 };
            if (!area_intersect(&clip, &save, &ba)) continue;
            const uint16_t *under = sc->under + (band ? sc->nt * w : 0);
            uint16_t *comp = sc->comp + (band ? sc->nt * w : 0);
            for (int32_t r = 0; r < rows; r++) {
                int32_t cr = sy + r - y0;
                const uint16_t *u = under + r * w;
                uint16_t *o = comp + r * w;
                if (cr < 0 || cr >= sc->h) { memcpy(o, u, w * 2); continue; }
                const uint16_t *src = sc->px + cr * w;
                for (int32_t x = 0; x < w; x++) o[x] = src[x] == bg ? u[x] : src[x];
            }
            lv_image_dsc_t *im = band ? &sc->img_b : &sc->img_t;
            *im = sc->img;
            im->header.h = rows;
            im->data = (const uint8_t *)comp;
            im->data_size = (uint32_t)w * rows * 2;
            lv_draw_image_dsc_init(&d);
            d.src = im;
            layer->_clip_area = clip;
            lv_draw_image(layer, &d, &ba);
        }
        /* Scrollleiste (sonst in DRAW_POST, das bei übersprungenem Inhalt entfällt) */
        lv_area_t hor, ver;
        lv_obj_get_scrollbar_area(c, &hor, &ver);
        if (lv_area_get_size(&ver) > 0 && area_intersect(&clip, &save, &ca)) {
            layer->_clip_area = clip;
            lv_draw_rect_dsc_t r;
            lv_draw_rect_dsc_init(&r);
            lv_obj_init_draw_rect_dsc(c, LV_PART_SCROLLBAR, &r);
            lv_draw_rect(layer, &r, &ver);
        }
        layer->_clip_area = save;
        layer->opa = LV_OPA_TRANSP;   /* Kinder überspringen; lv_obj_refr stellt die Deckkraft danach wieder her */
        TMARK(sw_t_sc1);
    }
}

static void scache_attach(lv_obj_t *o)
{
    scache_t *sc = lv_malloc_zeroed(sizeof(*sc));
    sc->cont = o;
    sc->born = lv_tick_get();
    sc->next = s_scaches;
    s_scaches = sc;
    lv_obj_add_event_cb(o, scache_event, LV_EVENT_ALL, sc);
    if (!s_sc_timer) s_sc_timer = lv_timer_create(sc_timer_cb, 20, NULL);
}

/* ------------------------------------------------------------------ Aufbau */

void sw_render_set_action_cb(sw_action_cb_t cb)
{
    s_action_cb = cb;
}

static void scroll_style(lv_obj_t *o)
{
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_width(o, 6, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(o, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(o, lv_color_hex(0xbbbeb5), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(o, 2, LV_PART_SCROLLBAR);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLL_ELASTIC);
}

static bool in_subtree(const sw_screen_def_t *def, int i, int root)
{
    while (i >= 0) {
        if (i == root) return true;
        i = def->nodes[i].parent;
    }
    return false;
}

static sw_screen_t *build(const sw_screen_def_t *def, lv_obj_t *parent, int only_root)
{
    sw_screen_t *s = lv_malloc_zeroed(sizeof(*s));
    s->def = def;
    s->objs = lv_malloc_zeroed(sizeof(lv_obj_t *) * def->count);
    s->root = lv_obj_create(parent);
    lv_obj_remove_style_all(s->root);
    lv_obj_set_size(s->root, 800, 480);
    if (only_root < 0) {
        lv_obj_add_event_cb(s->root, root_event, LV_EVENT_DRAW_MAIN, NULL);
    }
    lv_obj_remove_flag(s->root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (uint16_t i = 0; i < def->count; i++) {
        const sw_node_t *n = &def->nodes[i];
        if (only_root >= 0 && !in_subtree(def, i, only_root)) continue;
        lv_obj_t *p = n->parent < 0 ? s->root : s->objs[n->parent];
        lv_obj_t *o = NULL;
        switch (n->kind) {
        case SW_N_BOX:
        case SW_N_SWITCH: {
            o = lv_obj_create(p);
            lv_obj_remove_style_all(o);
            lv_obj_set_pos(o, n->x, n->y);
            lv_obj_set_size(o, n->w, n->h);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
            if (n->scroll) {
                lv_obj_add_flag(o, LV_OBJ_FLAG_SCROLLABLE);
                scroll_style(o);
                lv_obj_set_style_pad_bottom(o, n->pad_bottom, 0);
            }
            box_rt_t *rt = lv_malloc_zeroed(sizeof(*rt));
            rt->scr = s;
            rt->idx = i;
            lv_obj_add_event_cb(o, box_event, LV_EVENT_ALL, rt);
            if (n->scroll) scache_attach(o);
            if (n->action) lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
            else lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
            if (n->selected) lv_obj_add_state(o, LV_STATE_CHECKED);
            if (n->disabled) lv_obj_add_state(o, LV_STATE_DISABLED);
            if (n->st) {
                lv_obj_set_style_opa_layered(o, cur_style(o, rt)->opacity, 0);
            } else if (n->opa < 255) {
                lv_obj_set_style_opa_layered(o, n->opa, 0);
            }
            lv_obj_refresh_ext_draw_size(o);
            break;
        }
        case SW_N_LABEL:
            o = sw_text_create(p, n->font, n->ls_q4, n->align, n->x, n->y, n->text);
            lv_obj_set_style_text_color(o, n->color, 0);
            lv_obj_set_style_text_opa(o, n->opa, 0);
            break;
        case SW_N_ICON:
        case SW_N_IMG:
            o = lv_image_create(p);
            lv_obj_remove_style_all(o);
            lv_image_set_src(o, n->img);
            lv_obj_set_pos(o, n->x, n->y);
            if (n->kind == SW_N_ICON) {
                lv_obj_set_style_image_recolor(o, n->color, 0);
                lv_obj_set_style_image_recolor_opa(o, LV_OPA_COVER, 0);
            }
            lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
            break;
        case SW_N_RANGE: {
            o = lv_obj_create(p);
            lv_obj_remove_style_all(o);
            lv_obj_set_pos(o, n->x, n->y);
            lv_obj_set_size(o, n->w, n->h);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE);
            lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
            range_rt_t *r = lv_malloc_zeroed(sizeof(*r));
            r->n = n;
            r->value = n->value;
            lv_obj_add_event_cb(o, range_event, LV_EVENT_ALL, r);
            break;
        }
        case SW_N_DIVIDER: {   /* 1px Linie + box-shadow 1px 0 #fff */
            o = lv_obj_create(p);
            lv_obj_remove_style_all(o);
            lv_obj_set_pos(o, n->x, n->y);
            lv_obj_set_size(o, 2, n->h);
            lv_obj_set_style_bg_color(o, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
            lv_obj_set_style_border_side(o, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_border_width(o, 1, 0);
            lv_obj_set_style_border_color(o, n->color, 0);
            lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
            break;
        }
        }
        if (o && n->hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        s->objs[i] = o;
    }
    return s;
}

sw_screen_t *sw_screen_build(const sw_screen_def_t *def, lv_obj_t *parent)
{
    return build(def, parent, -1);
}

sw_screen_t *sw_overlay_build(const sw_screen_def_t *def, lv_obj_t *parent, int root_idx)
{
    sw_screen_t *s = build(def, parent, root_idx);
    if (s->objs[root_idx]) lv_obj_add_flag(s->objs[root_idx], LV_OBJ_FLAG_CLICKABLE); /* blockiert darunter (inert) */
    return s;
}

/* Test: Objekt mit dieser Aktion als gedrückt darstellen */
void sw_debug_press(sw_screen_t *s, const char *action)
{
    int i = sw_find_action(s->def, action);
    lv_obj_t *o = sw_obj(s, i);
    if (!o) return;
    lv_obj_add_state(o, LV_STATE_PRESSED);
    box_rt_t *rt = rt_of(o);
    if (rt) apply_variant(o, rt);
}

int sw_key(const sw_screen_def_t *def, const char *key)
{
    for (int i = 0; i < def->count; i++)
        if (def->nodes[i].key && !strcmp(def->nodes[i].key, key)) return i;
    return -1;
}

int sw_find_text(const sw_screen_def_t *def, const char *text)
{
    for (int i = 0; i < def->count; i++)
        if (def->nodes[i].kind == SW_N_LABEL && def->nodes[i].text && !strcmp(def->nodes[i].text, text)) return i;
    return -1;
}

int sw_find_action(const sw_screen_def_t *def, const char *action)
{
    for (int i = 0; i < def->count; i++)
        if (def->nodes[i].action && !strcmp(def->nodes[i].action, action)) return i;
    return -1;
}

int sw_find_text_in(const sw_screen_def_t *def, int container, const char *text)
{
    for (int i = 0; i < def->count; i++)
        if (def->nodes[i].kind == SW_N_LABEL && def->nodes[i].parent == container && def->nodes[i].text
            && !strcmp(def->nodes[i].text, text)) return i;
    return -1;
}

/* Breite einer Box ändern, Mitte beibehalten (Toast passt sich dem Text an) */
void sw_set_width_centered(sw_screen_t *s, int idx, int32_t w)
{
    lv_obj_t *o = sw_obj(s, idx);
    if (!o) return;
    const sw_node_t *n = &s->def->nodes[idx];
    lv_obj_set_x(o, n->x + n->w / 2 - w / 2);
    lv_obj_set_width(o, w);
    /* zentrierte Texte im Container mitführen (ihr Bezugspunkt liegt auf der alten Mitte) */
    for (uint16_t i = 0; i < s->def->count; i++) {
        const sw_node_t *c = &s->def->nodes[i];
        if (c->parent == idx && s->objs[i] && c->kind == SW_N_LABEL) lv_obj_set_x(s->objs[i], (w - n->w) / 2);
    }
}

void sw_screen_free(sw_screen_t *s)
{
    if (!s) return;
    lv_obj_delete(s->root);   /* box_rt_t und Texte geben sich über LV_EVENT_DELETE selbst frei */
    lv_free(s->objs);
    lv_free(s);
}

/* ------------------------------------------------------------------ Hilfen */

lv_obj_t *sw_obj(sw_screen_t *s, int idx)
{
    return (idx >= 0 && idx < s->def->count) ? s->objs[idx] : NULL;
}

void sw_set_text(sw_screen_t *s, int idx, const char *text)
{
    lv_obj_t *o = sw_obj(s, idx);
    if (o) sw_text_set(o, text);
}

void sw_set_textf(sw_screen_t *s, int idx, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    sw_set_text(s, idx, buf);
}

static box_rt_t *rt_of(lv_obj_t *o)
{
    for (uint32_t i = 0; i < lv_obj_get_event_count(o); i++) {
        lv_event_dsc_t *d = lv_obj_get_event_dsc(o, i);
        if (lv_event_dsc_get_cb(d) == box_event) return lv_event_dsc_get_user_data(d);
    }
    return NULL;
}

void sw_set_selected(sw_screen_t *s, int idx, bool sel)
{
    lv_obj_t *o = sw_obj(s, idx);
    if (!o) return;
    if (sel) lv_obj_add_state(o, LV_STATE_CHECKED);
    else lv_obj_remove_state(o, LV_STATE_CHECKED);
    box_rt_t *rt = rt_of(o);
    if (rt) apply_variant(o, rt);
}

bool sw_get_selected(sw_screen_t *s, int idx)
{
    lv_obj_t *o = sw_obj(s, idx);
    return o && lv_obj_has_state(o, LV_STATE_CHECKED);
}

void sw_set_disabled(sw_screen_t *s, int idx, bool dis)
{
    lv_obj_t *o = sw_obj(s, idx);
    if (!o) return;
    if (dis) lv_obj_add_state(o, LV_STATE_DISABLED);
    else lv_obj_remove_state(o, LV_STATE_DISABLED);
    /* button:disabled { opacity: .4 } */
    box_rt_t *rt = rt_of(o);
    if (rt) apply_variant(o, rt);
    sw_render_touch(o);
    lv_obj_set_style_opa_layered(o, dis ? 102 : (rt && cur_style(o, rt) ? cur_style(o, rt)->opacity : 255), 0);
}

void sw_set_hidden(sw_screen_t *s, int idx, bool hidden)
{
    lv_obj_t *o = sw_obj(s, idx);
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    sw_render_touch(o);
}

void sw_set_icon(sw_screen_t *s, int idx, const lv_image_dsc_t *img)
{
    lv_obj_t *o = sw_obj(s, idx);
    if (!o) return;
    lv_image_set_src(o, img);
    sw_render_touch(o);
}

void sw_set_color(sw_screen_t *s, int idx, lv_color_t c)
{
    lv_obj_t *o = sw_obj(s, idx);
    if (!o) return;
    if (s->def->nodes[idx].kind == SW_N_LABEL) lv_obj_set_style_text_color(o, c, 0);
    else lv_obj_set_style_image_recolor(o, c, 0);
    sw_render_touch(o);
}

void sw_set_box_style(sw_screen_t *s, int idx, const sw_style_t *st)
{
    lv_obj_t *o = sw_obj(s, idx);
    box_rt_t *rt = o ? rt_of(o) : NULL;
    if (!rt) return;
    rt->override = st;
    lv_obj_refresh_ext_draw_size(o);
    apply_variant(o, rt);
}

void sw_set_opa(sw_screen_t *s, int idx, lv_opa_t opa)
{
    lv_obj_t *o = sw_obj(s, idx);
    if (!o) return;
    lv_obj_set_style_opa_layered(o, opa, 0);
    sw_render_touch(o);
}

void sw_set_height_from_bottom(sw_screen_t *s, int idx, int32_t h)
{
    lv_obj_t *o = sw_obj(s, idx);
    if (!o) return;
    const sw_node_t *n = &s->def->nodes[idx];
    int32_t bottom = n->y + n->h;
    sw_render_touch(o);
    lv_obj_set_y(o, bottom - h);
    lv_obj_set_height(o, h);
    lv_obj_update_layout(o);
    sw_render_touch(o);
}
