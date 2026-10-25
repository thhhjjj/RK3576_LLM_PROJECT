/* SPDX-License-Identifier: GPL-2.0 */
#ifndef LLM_PANEL_UI_H
#define LLM_PANEL_UI_H

#include "metrics.h"

void ui_create(void);
void ui_update_metrics(const struct llm_metrics *m);
void ui_show_overtemp(int temp_c);
void ui_clear_overtemp(void);

void ui_set_dialog(const char *text);
void ui_append_dialog(const char *text);
void ui_set_ctx_tps(int ctx, int max_ctx, float tps);
void ui_set_model_status(const char *text);
void ui_set_ws_status(const char *text);
/* PHASE7-E / PHASE6: present/s; fps<0 → "—" */
void ui_set_fps(float fps);

/* 0 = local, 1 = cloud. */
int ui_mode_is_cloud(void);

/* Optional hooks installed by main (may be NULL). */
typedef void (*ui_action_fn)(void);
typedef void (*ui_mode_fn)(int cloud);
void ui_set_actions(ui_action_fn on_compress, ui_action_fn on_ask);
void ui_set_mode_hook(ui_mode_fn on_mode);

#endif
