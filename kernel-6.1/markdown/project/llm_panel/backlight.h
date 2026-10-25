/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_PANEL_BACKLIGHT_H
#define LLM_PANEL_BACKLIGHT_H

/* 亮度百分数 20～100；写 sysfs backlight 的 brightness 节点 */
void backlight_init(void);
int backlight_get_pct(void);
int backlight_set_pct(int pct);

#endif
