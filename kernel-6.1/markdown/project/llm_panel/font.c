/* SPDX-License-Identifier: GPL-2.0 */
/*
 * FreeType + Source Han Sans CN Regular (Adobe SubsetOTF).
 * Path matches Rockchip lvgl_demo convention.
 */

#include "font.h"

#include <stdio.h>

#define FONT_PATH \
	"/usr/share/fonts/source-han-sans-cn/SourceHanSansCN-Regular.otf"

static lv_ft_info_t ft_title;
static lv_ft_info_t ft_body;
static lv_ft_info_t ft_small;
static int ready;

int font_init(void)
{
	if (!lv_freetype_init(64, 1, 0)) {
		fprintf(stderr, "llm_panel: lv_freetype_init failed\n");
		return -1;
	}

	ft_title.name = FONT_PATH;
	ft_title.weight = 22;
	ft_title.style = FT_FONT_STYLE_NORMAL;
	if (!lv_ft_font_init(&ft_title)) {
		fprintf(stderr, "llm_panel: font title init failed (%s)\n",
			FONT_PATH);
		return -1;
	}

	ft_body.name = FONT_PATH;
	ft_body.weight = 16;
	ft_body.style = FT_FONT_STYLE_NORMAL;
	if (!lv_ft_font_init(&ft_body)) {
		fprintf(stderr, "llm_panel: font body init failed\n");
		return -1;
	}

	ft_small.name = FONT_PATH;
	ft_small.weight = 14;
	ft_small.style = FT_FONT_STYLE_NORMAL;
	if (!lv_ft_font_init(&ft_small)) {
		fprintf(stderr, "llm_panel: font small init failed\n");
		return -1;
	}

	ready = 1;
	printf("llm_panel: Chinese font ready (%s)\n", FONT_PATH);
	return 0;
}

const lv_font_t *font_title(void)
{
	return ready && ft_title.font ? ft_title.font : &lv_font_montserrat_20;
}

const lv_font_t *font_body(void)
{
	return ready && ft_body.font ? ft_body.font : &lv_font_montserrat_14;
}

const lv_font_t *font_small(void)
{
	return ready && ft_small.font ? ft_small.font : &lv_font_montserrat_14;
}
