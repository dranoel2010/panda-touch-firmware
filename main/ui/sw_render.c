/**
 * Soft White – Zeichen-Engine.
 *
 * Jede Box wird in derselben Reihenfolge gezeichnet wie im Browser:
 *   äußere Schatten (Masken, letzter zuerst) -> Hintergrund/Verlauf -> innere Schatten -> Rahmen -> Inhalt.
 * Schatten sind Alpha-Masken aus Chrome, verlustfrei gekürzt; das Mittelband wird gekachelt.
 */
#include "sw_render.h"
#include "sw_text.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sw_action_cb_t s_action_cb;

typedef struct {
    sw_screen_t *scr;
    int idx;
    const sw_style_t *override;
} box_rt_t;

/* ------------------------------------------------------------------ Masken-Stücke */

/* Zu jeder Maske 9 dauerhafte Bildbeschreibungen (LVGL zeichnet verzögert, darf nicht auf den Stack zeigen). */
typedef struct piece_cache {
    const sw_mask_t *m;
    lv_image_dsc_t p[9];
    struct piece_cache *next;
} piece_cache_t;
static piece_cache_t *s_pieces;

static lv_image_dsc_t *pieces_for(const sw_mask_t *m)
{
    for (piece_cache_t *c = s_pieces; c; c = c->next) {
        if (c->m == m) return c->p;
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
        }
    }
    c->next = s_pieces;
    s_pieces = c;
    return c->p;
}

/* Maske in Zielgröße W×H ab (x,y) zeichnen */
static void draw_mask(lv_layer_t *layer, const sw_mask_t *m, int32_t x, int32_t y, int32_t W, int32_t H,
                      lv_color_t color, lv_opa_t opa)
{
    if (opa <= LV_OPA_MIN) return;
    lv_image_dsc_t *p = pieces_for(m);
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
            if (!img->header.w || !img->header.h) continue;
            lv_draw_image_dsc_t d;
            lv_draw_image_dsc_init(&d);
            d.src = img;
            d.recolor = color;
            d.recolor_opa = LV_OPA_COVER;
            d.opa = opa;
            if (k == 1 || r == 1) {
                d.tile = 1;
                d.image_area = a;
            }
            lv_draw_image(layer, &d, &a);
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
    for (int i = st->n_shadows - 1; i >= 0; i--) {   /* CSS: erster Schatten liegt oben */
        const sw_shadow_t *sh = &st->shadows[i];
        if (sh->inset != inset) continue;
        int32_t w = lv_area_get_width(c), h = lv_area_get_height(c);
        draw_mask(layer, sh->mask, c->x1 + sh->mask->ox, c->y1 + sh->mask->oy,
                  w + sh->mask->grow_w, h + sh->mask->grow_h, sh->color, sh->opa);
    }
}

static void draw_body(lv_layer_t *layer, const sw_style_t *st, const lv_area_t *c)
{
    if (st->bg == SW_BG_NONE) return;
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
    if (m) draw_mask(layer, m, k.x1 + m->ox, k.y1 + m->oy, 40 + m->grow_w, 40 + m->grow_h, lv_color_hex(0x78847a), 0x70);
    static const sw_style_t knob = { SW_BG_LINEAR, LV_COLOR_MAKE(0xf4, 0xfa, 0xf4), LV_COLOR_MAKE(0xe4, 0xe6, 0xe4),
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
        lv_obj_set_style_translate_y(obj, st->translate_y, 0);
        for (uint16_t i = 0; i < s->def->count; i++) {
            const sw_node_t *c = &s->def->nodes[i];
            if (c->parent != rt->idx || !c->inherit_color || !s->objs[i]) continue;
            if (c->kind == SW_N_LABEL) lv_obj_set_style_text_color(s->objs[i], st->text_color, 0);
            else if (c->kind == SW_N_ICON) lv_obj_set_style_image_recolor(s->objs[i], st->text_color, 0);
        }
    }
    lv_obj_invalidate(obj);
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
        lv_obj_set_style_bg_color(s->root, SW_SURFACE, 0);
        lv_obj_set_style_bg_opa(s->root, LV_OPA_COVER, 0);
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
    lv_obj_set_style_opa_layered(o, dis ? 102 : (rt && cur_style(o, rt) ? cur_style(o, rt)->opacity : 255), 0);
}

void sw_set_hidden(sw_screen_t *s, int idx, bool hidden)
{
    lv_obj_t *o = sw_obj(s, idx);
    if (!o) return;
    if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void sw_set_icon(sw_screen_t *s, int idx, const lv_image_dsc_t *img)
{
    lv_obj_t *o = sw_obj(s, idx);
    if (o) lv_image_set_src(o, img);
}

void sw_set_color(sw_screen_t *s, int idx, lv_color_t c)
{
    lv_obj_t *o = sw_obj(s, idx);
    if (!o) return;
    if (s->def->nodes[idx].kind == SW_N_LABEL) lv_obj_set_style_text_color(o, c, 0);
    else lv_obj_set_style_image_recolor(o, c, 0);
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
    if (o) lv_obj_set_style_opa_layered(o, opa, 0);
}

void sw_set_height_from_bottom(sw_screen_t *s, int idx, int32_t h)
{
    lv_obj_t *o = sw_obj(s, idx);
    if (!o) return;
    const sw_node_t *n = &s->def->nodes[idx];
    int32_t bottom = n->y + n->h;
    lv_obj_set_y(o, bottom - h);
    lv_obj_set_height(o, h);
}
