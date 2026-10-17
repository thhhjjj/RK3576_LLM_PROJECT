/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_PANEL_FONT_H
#define LLM_PANEL_FONT_H

#include "lvgl/lvgl.h"

/* Returns 0 on success. On failure UI may fall back to montserrat. */
int font_init(void);

const lv_font_t *font_title(void);
const lv_font_t *font_body(void);
const lv_font_t *font_small(void);

#endif
