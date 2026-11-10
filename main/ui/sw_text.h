/**
 * Soft White – Text mit Subpixel-Laufweite (wie Chrome), siehe sw_text.c
 */
#pragma once
#include "sw_types.h"

lv_obj_t *sw_text_create(lv_obj_t *parent, const lv_font_t *font, int32_t ls_q4, uint8_t align, int32_t anchor_q4,
                         int32_t y, const char *text);
void sw_text_set(lv_obj_t *o, const char *text);
const char *sw_text_get(lv_obj_t *o);
int32_t sw_text_width_q4(const lv_font_t *font, const char *txt, int32_t ls_q4);
